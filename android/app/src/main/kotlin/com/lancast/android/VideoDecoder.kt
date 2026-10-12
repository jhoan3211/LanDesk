package com.lancast.android

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.view.Surface
import com.lancast.core.AssembledFrame
import com.lancast.core.Codec
import com.lancast.core.StreamStarted
import java.nio.ByteBuffer
import java.util.ArrayDeque

// Decodificador por hardware (MediaCodec, modo asíncrono) que dibuja directo en la Surface del video.
// Configurado para la menor latencia: cada cuadro se presenta apenas sale del decodificador.
class VideoDecoder(
    private val surface: Surface,
    private val needKeyframe: () -> Unit,
    private val onError: (String) -> Unit,
) {
    private val thread = HandlerThread("lancast-decoder", Process.THREAD_PRIORITY_URGENT_DISPLAY).apply { start() }
    private val handler = Handler(thread.looper)
    private val lock = Any()
    private var codec: MediaCodec? = null
    private var configured: StreamStarted? = null
    private val pending = ArrayDeque<AssembledFrame>()
    private val freeInputs = ArrayDeque<Int>()
    private val queuedAt = HashMap<Long, Long>()
    private var awaitingKey = false  // Solo en el hilo del decodificador.

    @Volatile var decodeMs = 0.0
        private set
    @Volatile var name = ""
        private set

    // Desde el hilo de red: entrega un cuadro completo.
    fun submit(frame: AssembledFrame, stream: StreamStarted) {
        handler.post {
            val cfg = configured
            if (codec == null || cfg == null || cfg.epoch != stream.epoch || cfg.codec != stream.codec ||
                cfg.width != stream.width || cfg.height != stream.height
            ) {
                // Configuración nueva: hay que empezar por un keyframe.
                if (!frame.info.keyframe) {
                    needKeyframe()
                    return@post
                }
                if (!configure(stream, frame.data)) return@post
                awaitingKey = false
            }
            if (awaitingKey) {
                if (!frame.info.keyframe) return@post
                awaitingKey = false
            }
            var overflow = false
            synchronized(lock) {
                pending.addLast(frame)
                // El decodificador quedó muy atrás: saltar cuadros sueltos dañaría la imagen, así que se descarta
                // todo y se pide un keyframe (vuelve a estar al día de inmediato).
                if (pending.size > 6) {
                    pending.clear()
                    overflow = true
                }
            }
            if (overflow) {
                awaitingKey = true
                needKeyframe()
                return@post
            }
            feed()
        }
    }

    private fun format(stream: StreamStarted, keyframe: ByteArray, operatingRate: Boolean): MediaFormat {
        val mime = DeviceInfo.mime(stream.codec)
        val format = MediaFormat.createVideoFormat(mime, stream.width, stream.height)
        csd(stream.codec, keyframe).forEachIndexed { i, b -> format.setByteBuffer("csd-$i", ByteBuffer.wrap(b)) }
        if (Build.VERSION.SDK_INT >= 30) format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        format.setInteger(MediaFormat.KEY_PRIORITY, 0)  // Tiempo real.
        if (operatingRate) format.setInteger(MediaFormat.KEY_OPERATING_RATE, Short.MAX_VALUE.toInt())
        // Modos de baja latencia propios de cada fabricante (se ignoran si no existen).
        format.setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
        format.setInteger("vendor.qti-ext-dec-picture-order.enable", 1)
        format.setInteger("vendor.rtc-ext-dec-low-latency.enable", 1)
        format.setInteger("vendor.low-latency.enable", 1)
        format.setInteger("vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-req", 1)
        return format
    }

    private fun configure(stream: StreamStarted, keyframe: ByteArray): Boolean {
        release()
        return try {
            val c = MediaCodec.createDecoderByType(DeviceInfo.mime(stream.codec))
            name = c.name
            c.setCallback(callback, handler)
            try {
                c.configure(format(stream, keyframe, true), surface, null, 0)
            } catch (e: Exception) {
                // Algún decodificador rechaza KEY_OPERATING_RATE alto: reintentar sin él.
                c.reset()
                c.setCallback(callback, handler)
                c.configure(format(stream, keyframe, false), surface, null, 0)
            }
            c.start()
            codec = c
            configured = stream
            true
        } catch (e: Exception) {
            onError("No se pudo iniciar el decodificador ${stream.codec.label}: ${e.message}")
            release()
            false
        }
    }

    private val callback = object : MediaCodec.Callback() {
        override fun onInputBufferAvailable(c: MediaCodec, index: Int) {
            if (c !== codec) return
            synchronized(lock) { freeInputs.addLast(index) }
            feed()
        }

        override fun onOutputBufferAvailable(c: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
            if (c !== codec) return
            queuedAt.remove(info.presentationTimeUs)?.let { decodeMs = (System.nanoTime() / 1000 - it) / 1000.0 }
            try {
                c.releaseOutputBuffer(index, true)  // A la pantalla en el próximo refresco.
            } catch (_: Exception) {
            }
        }

        override fun onError(c: MediaCodec, e: MediaCodec.CodecException) {
            if (c !== codec) return
            release()
            needKeyframe()
        }

        override fun onOutputFormatChanged(c: MediaCodec, format: MediaFormat) {}
    }

    private fun feed() {
        val c = codec ?: return
        while (true) {
            val frame: AssembledFrame
            val index: Int
            synchronized(lock) {
                if (pending.isEmpty() || freeInputs.isEmpty()) return
                frame = pending.removeFirst()
                index = freeInputs.removeFirst()
            }
            try {
                val buf = c.getInputBuffer(index) ?: continue
                buf.clear()
                if (frame.data.size > buf.capacity()) {
                    needKeyframe()
                    c.queueInputBuffer(index, 0, 0, frame.info.captureTimeUs, 0)
                    continue
                }
                buf.put(frame.data)
                val pts = frame.info.captureTimeUs
                if (queuedAt.size > 64) queuedAt.clear()
                queuedAt[pts] = System.nanoTime() / 1000
                c.queueInputBuffer(index, 0, frame.data.size, pts, if (frame.info.keyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
            } catch (e: Exception) {
                release()
                needKeyframe()
                return
            }
        }
    }

    private fun release() {
        val c = codec
        codec = null
        configured = null
        synchronized(lock) {
            pending.clear()
            freeInputs.clear()
        }
        queuedAt.clear()
        try {
            c?.stop()
        } catch (_: Exception) {
        }
        try {
            c?.release()
        } catch (_: Exception) {
        }
    }

    // Espera a que el decodificador suelte la Surface (se llama cuando la Surface está por destruirse).
    fun close() {
        val done = java.util.concurrent.CountDownLatch(1)
        handler.post {
            release()
            done.countDown()
        }
        done.await(500, java.util.concurrent.TimeUnit.MILLISECONDS)
        thread.quitSafely()
    }

    companion object {
        // Parámetros del códec (SPS/PPS, y VPS en HEVC) tomados del keyframe, en formato Annex-B.
        fun csd(codec: Codec, data: ByteArray): List<ByteArray> {
            val nals = splitAnnexB(data)
            return when (codec) {
                Codec.H264 -> {
                    val sps = nals.firstOrNull { (it[startLen(it)].toInt() and 0x1F) == 7 }
                    val pps = nals.firstOrNull { (it[startLen(it)].toInt() and 0x1F) == 8 }
                    if (sps != null && pps != null) listOf(sps, pps) else emptyList()
                }
                Codec.HEVC -> {
                    val params = nals.filter { ((it[startLen(it)].toInt() shr 1) and 0x3F) in 32..34 }
                    if (params.size >= 3) listOf(params.fold(ByteArray(0)) { acc, b -> acc + b }) else emptyList()
                }
                Codec.AV1 -> emptyList()  // AV1 lleva la cabecera de secuencia dentro del cuadro.
            }
        }

        private fun startLen(nal: ByteArray) = if (nal.size > 3 && nal[2].toInt() == 1) 3 else 4

        // Corta el flujo en unidades NAL conservando el código de inicio de cada una.
        fun splitAnnexB(data: ByteArray): List<ByteArray> {
            val starts = ArrayList<Int>()
            var i = 0
            while (i + 3 <= data.size) {
                if (data[i].toInt() == 0 && data[i + 1].toInt() == 0) {
                    if (data[i + 2].toInt() == 1) {
                        starts += i
                        i += 3
                        continue
                    }
                    if (i + 4 <= data.size && data[i + 2].toInt() == 0 && data[i + 3].toInt() == 1) {
                        starts += i
                        i += 4
                        continue
                    }
                }
                i++
            }
            return starts.mapIndexedNotNull { n, s ->
                val e = if (n + 1 < starts.size) starts[n + 1] else data.size
                val nal = data.copyOfRange(s, e)
                if (nal.size > startLen(nal)) nal else null
            }
        }
    }
}
