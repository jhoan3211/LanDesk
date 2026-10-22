package com.lancast.android

import android.app.Activity
import android.content.Intent
import android.os.Build
import com.lancast.core.StatSample
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// Registro corto de lo que pasó (cortes, errores, compartir pantalla) y un texto para soporte que se comparte con
// cualquier app (correo, WhatsApp...). No incluye claves ni emparejamientos.
object AppLog {
    private val lines = ArrayDeque<String>()
    private val time = SimpleDateFormat("HH:mm:ss", Locale.ROOT)

    @Volatile var lastStats: List<StatSample> = emptyList()
    @Volatile var lastStream: String = ""

    fun add(text: String) = synchronized(lines) {
        lines.addLast("${time.format(Date())} $text")
        while (lines.size > 300) lines.removeFirst()
    }

    fun recent(): List<String> = synchronized(lines) { lines.toList() }
}

object Diagnostics {
    fun build(activity: Activity, storage: Storage): String = buildString {
        fun line(s: String = "") = append(s).append('\n')
        line("LanCast ${Updater.currentVersion(activity)} · diagnóstico del ${SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.ROOT).format(Date())}")
        line()
        line("== Teléfono ==")
        line("${Build.MANUFACTURER} ${Build.MODEL} · Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
        val (display, hz) = DeviceInfo.display(activity)
        line("Pantalla ${display.width}x${display.height} @ $hz Hz · IP ${DeviceInfo.localIp() ?: "?"}")
        line("Decodificadores por hardware: ${DeviceInfo.hardwareDecoders().joinToString { it.label }}")
        line("Codificadores por hardware: ${ScreenEncoder.availableCodecs().joinToString { it.label }}")
        line("Control remoto (accesibilidad): ${if (ControlService.enabled()) "activado" else "desactivado"}")
        val share = ShareService.current
        line("Compartiendo: ${if (share.sharing) "sí" else "no"}${share.client?.let { " · con $it" } ?: ""} ${share.detail}")
        line()
        line("== Ajustes ==")
        line("Resolución ${storage.resolution} · FPS ${storage.fps} · bitrate ${storage.bitrateMbps} Mbps · códec ${storage.codec} · " +
            "audio ${storage.audio} · ritmo ${storage.pacingMbps} Mbps · touchpad ${storage.touchpad}")
        line("Equipos emparejados: ${storage.paired().size} · equipos que controlan este teléfono: ${storage.clients().size}")
        line()
        line("== Última sesión ==")
        if (AppLog.lastStream.isNotEmpty()) line(AppLog.lastStream)
        val stats = AppLog.lastStats
        if (stats.isNotEmpty()) {
            val avg = { f: (StatSample) -> Float -> stats.map(f).average() }
            line("Últimos ${stats.size / 4} s: FPS %.0f · %.1f Mbps · red %.1f ms (picos %.1f) · RTT %.1f ms · perdidos %d · reparados %d".format(
                avg { it.fps }, avg { it.mbps }, avg { it.networkMs }, stats.maxOf { it.networkMaxMs }, avg { it.rttMs },
                stats.sumOf { it.lost }, stats.sumOf { it.recovered }))
        } else {
            line("Sin datos")
        }
        line()
        line("== Registro ==")
        AppLog.recent().forEach { line(it) }
    }

    fun share(activity: Activity, storage: Storage) {
        val text = build(activity, storage)
        activity.startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_SUBJECT, "Diagnóstico de LanCast")
            putExtra(Intent.EXTRA_TEXT, text)
        }, "Enviar diagnóstico"))
    }
}
