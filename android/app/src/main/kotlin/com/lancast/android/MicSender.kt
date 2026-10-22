package com.lancast.android

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import com.lancast.core.Session

// Micrófono del teléfono -> PC (48 kHz mono, trozos de 10 ms). Fuente de comunicación: con cancelación de eco, para
// que el sonido del PC que sale por el altavoz del teléfono no vuelva al PC.
class MicSender(private val session: () -> Session?) {
    @Volatile private var running = false
    private var thread: Thread? = null
    val active: Boolean get() = running

    // Requiere el permiso RECORD_AUDIO (lo pide StreamActivity antes).
    @SuppressLint("MissingPermission")
    fun start(): Boolean {
        if (running) return true
        val rate = 48000
        val min = AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        if (min <= 0) return false
        val rec = try {
            AudioRecord(MediaRecorder.AudioSource.VOICE_COMMUNICATION, rate, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT, maxOf(min, rate / 10 * 2))
        } catch (_: Exception) {
            return false
        }
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            rec.release()
            return false
        }
        running = true
        thread = Thread({
            val buf = ShortArray(rate / 100)
            try {
                rec.startRecording()
                while (running) {
                    val n = rec.read(buf, 0, buf.size)
                    if (n > 0) session()?.sendMic(buf, n, rate)
                    else if (n < 0) break
                }
            } catch (_: Exception) {
            } finally {
                runCatching { rec.stop() }
                rec.release()
                running = false
            }
        }, "LanCast-mic").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
        AppLog.add("Micrófono hacia el PC: activado")
        return true
    }

    fun stop() {
        if (!running && thread == null) return
        running = false
        thread?.join(500)
        thread = null
        AppLog.add("Micrófono hacia el PC: apagado")
    }
}
