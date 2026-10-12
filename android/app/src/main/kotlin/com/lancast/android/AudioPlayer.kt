package com.lancast.android

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack

// Audio PCM del host con la menor latencia posible: AudioTrack de baja latencia y un colchón corto; si se
// acumula más de lo previsto (el teléfono reproduce más lento), se descartan muestras en vez de atrasarse.
class AudioPlayer {
    private var track: AudioTrack? = null
    private var rate = 0
    private var channels = 0
    private var written = 0L
    @Volatile var volume = 1f

    fun play(pcm: ShortArray, sampleRate: Int, channelCount: Int) {
        if (track == null || sampleRate != rate || channelCount != channels) open(sampleRate, channelCount)
        val t = track ?: return
        // Colchón: ~60 ms. Más que eso es latencia acumulada.
        val queued = written - t.playbackHeadPosition.toLong()
        if (queued > sampleRate * 60L / 1000) return
        val n = t.write(pcm, 0, pcm.size, AudioTrack.WRITE_NON_BLOCKING)
        if (n > 0) written += n / channelCount
    }

    private fun open(sampleRate: Int, channelCount: Int) {
        close()
        rate = sampleRate
        channels = channelCount
        val mask = if (channelCount == 1) AudioFormat.CHANNEL_OUT_MONO else AudioFormat.CHANNEL_OUT_STEREO
        val min = AudioTrack.getMinBufferSize(sampleRate, mask, AudioFormat.ENCODING_PCM_16BIT)
        track = try {
            AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MOVIE).build()
                )
                .setAudioFormat(AudioFormat.Builder().setSampleRate(sampleRate).setChannelMask(mask).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(maxOf(min, sampleRate * channelCount * 2 / 25))  // >= 40 ms
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
                .also {
                    it.setVolume(volume)
                    it.play()
                }
        } catch (_: Exception) {
            null
        }
        written = 0
    }

    fun close() {
        try {
            track?.stop()
        } catch (_: Exception) {
        }
        track?.release()
        track = null
    }
}
