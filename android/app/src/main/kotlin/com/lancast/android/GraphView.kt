package com.lancast.android

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.view.View
import com.lancast.core.StatSample
import kotlin.math.max

// Gráficos en tiempo real (últimos 30 s): latencia de red (promedio y picos), decodificación, Mbps y FPS, con
// marcas rojas donde se perdieron cuadros y amarillas donde los reparó la corrección de errores.
class GraphView(context: Context) : View(context) {
    private var samples: List<StatSample> = emptyList()
    private var decode: List<Float> = emptyList()
    private val density = resources.displayMetrics.density
    private val bg = Paint().apply { color = 0xB0000000.toInt() }
    private val grid = Paint().apply {
        color = 0x33FFFFFF
        strokeWidth = density
    }
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFE6E6EB.toInt()
        textSize = 11 * density
    }
    private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1.6f * density
    }
    private val mark = Paint().apply { strokeWidth = 2 * density }
    private val path = Path()

    fun update(samples: List<StatSample>, decodeMs: List<Float>) {
        this.samples = samples
        this.decode = decodeMs
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        canvas.drawRoundRect(0f, 0f, w, h, 8 * density, 8 * density, bg)
        if (samples.size < 2) {
            canvas.drawText("Reuniendo datos…", 10 * density, 20 * density, label)
            return
        }
        val rows = 4
        val pad = 6 * density
        val rowH = (h - pad) / rows
        fun series(row: Int, title: String, color: Int, values: List<Float>, extra: List<Float>? = null, extraColor: Int = 0, unit: String) {
            val top = pad + row * rowH
            val bottom = top + rowH - pad
            val left = pad
            val right = w - pad
            val maxV = max(values.maxOrNull() ?: 0f, extra?.maxOrNull() ?: 0f).let { if (it <= 0f) 1f else it * 1.15f }
            canvas.drawLine(left, bottom, right, bottom, grid)
            fun draw(v: List<Float>, c: Int) {
                if (v.size < 2) return
                line.color = c
                val p = path.apply { reset() }
                val step = (right - left) / (SLOTS - 1)
                val start = SLOTS - v.size
                v.forEachIndexed { i, value ->
                    val x = left + (start + i) * step
                    val y = bottom - (value / maxV) * (bottom - top - 14 * density)
                    if (i == 0) p.moveTo(x, y) else p.lineTo(x, y)
                }
                canvas.drawPath(p, line)
            }
            if (extra != null) draw(extra, extraColor)
            draw(values, color)
            val last = values.lastOrNull() ?: 0f
            val peak = (extra ?: values).maxOrNull() ?: 0f
            label.color = color
            canvas.drawText("%s  %.1f %s  (máx %.1f)".format(title, last, unit, peak), left + 2 * density, top + 11 * density, label)
        }
        series(0, "Red", 0xFF4A90E2.toInt(), samples.map { it.networkMs }, samples.map { it.networkMaxMs }, 0x804A90E2.toInt(), "ms")
        series(1, "Decodificación", 0xFFB07CE8.toInt(), decode, unit = "ms")
        series(2, "Bitrate", 0xFF4CC777.toInt(), samples.map { it.mbps }, unit = "Mbps")
        series(3, "FPS", 0xFFE8B34A.toInt(), samples.map { it.fps }, unit = "")
        // Pérdidas (rojo) y reparaciones con FEC (amarillo) sobre el gráfico de red.
        val step = (w - 2 * pad) / (SLOTS - 1)
        val start = SLOTS - samples.size
        samples.forEachIndexed { i, s ->
            val x = pad + (start + i) * step
            if (s.lost > 0) {
                mark.color = 0xFFD9534F.toInt()
                canvas.drawLine(x, pad, x, pad + rowH - pad, mark)
            } else if (s.recovered > 0) {
                mark.color = 0x80E8D44A.toInt()
                canvas.drawLine(x, pad + rowH * 0.6f, x, pad + rowH - pad, mark)
            }
        }
    }

    companion object {
        const val SLOTS = 120  // 30 s en ventanas de 250 ms.
    }
}
