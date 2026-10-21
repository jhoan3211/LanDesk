package com.lancast.android

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.view.Surface
import com.lancast.core.Codec
import com.lancast.core.Session

// Codificador por hardware alimentado directamente por la pantalla capturada (Surface de entrada: sin copias).
// Cada cuadro sale en Annex-B; los keyframes llevan delante los parámetros del códec (SPS/PPS/VPS).
class ScreenEncoder(
    val codec: Codec,
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrateKbps: Int,
    private val onFrame: (data: ByteArray, length: Int, keyframe: Boolean, captureUs: Long, encodeUs: Long) -> Unit,
) {
    private val mime = DeviceInfo.mime(codec)
    private val encoder: MediaCodec
    val surface: Surface
    val name: String
    @Volatile private var running = true
    private val thread: Thread
    private var config = ByteArray(0)

    init {
        val info = hardwareEncoder(mime) ?: throw IllegalStateException("Sin codificador por hardware para ${codec.label}")
        name = info.name
        encoder = MediaCodec.createByCodecName(info.name)
        val format = MediaFormat.createVideoFormat(mime, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrateKbps * 1000)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setFloat(MediaFormat.KEY_I_FRAME_INTERVAL, 3600f)  // Keyframes solo cuando el cliente los pide.
            // Con la pantalla quieta igual sale un cuadro cada 100 ms (el cliente sabe que la conexión sigue).
            setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000)
            setInteger(MediaFormat.KEY_PRIORITY, 0)  // Tiempo real.
            setInteger(MediaFormat.KEY_OPERATING_RATE, fps)
            if (Build.VERSION.SDK_INT >= 29) setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LATENCY, 1)
            val caps = info.getCapabilitiesForType(mime).encoderCapabilities
            if (caps?.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR) == true) {
                setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            }
        }
        encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        surface = encoder.createInputSurface()
        encoder.start()
        thread = Thread({ drain() }, "lancast-encoder").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    private fun drain() {
        val info = MediaCodec.BufferInfo()
        var out = ByteArray(256 * 1024)
        while (running) {
            val index = try {
                encoder.dequeueOutputBuffer(info, 50_000)
            } catch (_: Exception) {
                break
            }
            if (index < 0) continue
            try {
                val buf = encoder.getOutputBuffer(index)
                if (buf != null && info.size > 0) {
                    buf.position(info.offset)
                    buf.limit(info.offset + info.size)
                    if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                        config = ByteArray(info.size).also { buf.get(it) }
                    } else {
                        val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                        val prefix = if (key) config.size else 0
                        val total = prefix + info.size
                        if (out.size < total) out = ByteArray(total + total / 2)
                        if (prefix > 0) System.arraycopy(config, 0, out, 0, prefix)
                        buf.get(out, prefix, info.size)
                        // La marca de tiempo de la captura es del mismo reloj monotónico que usa la sesión.
                        val now = Session.nowUs()
                        onFrame(out, total, key, info.presentationTimeUs, (now - info.presentationTimeUs).coerceAtLeast(0))
                    }
                }
            } catch (_: Exception) {
            } finally {
                runCatching { encoder.releaseOutputBuffer(index, false) }
            }
        }
    }

    fun requestKeyframe() {
        runCatching { encoder.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) }) }
    }

    fun setBitrate(kbps: Int) {
        runCatching { encoder.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, kbps * 1000) }) }
    }

    fun release() {
        running = false
        thread.join(500)
        runCatching { encoder.stop() }
        runCatching { encoder.release() }
        surface.release()
    }

    companion object {
        fun hardwareEncoder(mime: String): MediaCodecInfo? =
            MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.firstOrNull { info ->
                info.isEncoder && DeviceInfo.isHardware(info) && info.supportedTypes.any { it.equals(mime, true) }
            }

        fun availableCodecs(): List<Codec> = listOf(Codec.H264, Codec.HEVC).filter { hardwareEncoder(DeviceInfo.mime(it)) != null }

        // Tamaño para el stream: la pantalla del teléfono dentro del máximo que pide el cliente (sin importar si el
        // teléfono está vertical u horizontal), pares, alineado a 16 y que el codificador acepte.
        fun fitSize(codec: Codec, screenW: Int, screenH: Int, maxW: Int, maxH: Int): Pair<Int, Int> {
            val longMax = maxOf(maxW, maxH).coerceAtLeast(320)
            val shortMax = minOf(maxW, maxH).coerceAtLeast(240)
            val long = maxOf(screenW, screenH).toDouble()
            val short = minOf(screenW, screenH).toDouble()
            var k = minOf(1.0, longMax / long, shortMax / short)
            val caps = hardwareEncoder(DeviceInfo.mime(codec))?.getCapabilitiesForType(DeviceInfo.mime(codec))?.videoCapabilities
            while (true) {
                val w = ((screenW * k).toInt() / 16 * 16).coerceAtLeast(16)
                val h = ((screenH * k).toInt() / 16 * 16).coerceAtLeast(16)
                if (caps == null || caps.isSizeSupported(w, h) || k < 0.2) return w to h
                k *= 0.9
            }
        }
    }
}
