package com.lancast.android

import android.app.Activity
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.os.Build
import android.util.Size
import com.lancast.core.Codec
import java.net.InetAddress
import java.net.NetworkInterface

object DeviceInfo {
    fun mime(codec: Codec) = when (codec) {
        Codec.H264 -> "video/avc"
        Codec.HEVC -> "video/hevc"
        Codec.AV1 -> "video/av01"
    }

    // Códecs que este teléfono decodifica por hardware (los de software no sirven para baja latencia).
    fun hardwareDecoders(): List<Codec> {
        val infos = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { !it.isEncoder && isHardware(it) }
        return Codec.entries.filter { c -> infos.any { info -> info.supportedTypes.any { it.equals(mime(c), true) } } }
    }

    fun isHardware(info: MediaCodecInfo): Boolean {
        if (Build.VERSION.SDK_INT >= 29) return info.isHardwareAccelerated && !info.isSoftwareOnly
        val n = info.name.lowercase()
        return !(n.startsWith("omx.google.") || n.startsWith("c2.android.") || n.contains("ffmpeg") || n.contains(".sw."))
    }

    // Pantalla en horizontal (ancho >= alto) y su frecuencia.
    fun display(activity: Activity): Pair<Size, Int> {
        val (w, h) = if (Build.VERSION.SDK_INT >= 30) {
            val b = activity.windowManager.maximumWindowMetrics.bounds
            b.width() to b.height()
        } else {
            val m = android.util.DisplayMetrics()
            @Suppress("DEPRECATION")
            activity.windowManager.defaultDisplay.getRealMetrics(m)
            m.widthPixels to m.heightPixels
        }
        @Suppress("DEPRECATION")
        val hz = (if (Build.VERSION.SDK_INT >= 30) activity.display?.refreshRate else activity.windowManager.defaultDisplay.refreshRate)
            ?: 60f
        return Size(maxOf(w, h), minOf(w, h)) to Math.round(hz).coerceIn(30, 240)
    }

    // Direcciones de broadcast de las redes del teléfono (para encontrar los PC).
    fun broadcastAddresses(): List<InetAddress> {
        val out = ArrayList<InetAddress>()
        try {
            for (ni in NetworkInterface.getNetworkInterfaces()) {
                if (!ni.isUp || ni.isLoopback) continue
                for (ia in ni.interfaceAddresses) ia.broadcast?.let { out += it }
            }
        } catch (_: Exception) {
        }
        out += InetAddress.getByName("255.255.255.255")
        return out.distinct()
    }
}
