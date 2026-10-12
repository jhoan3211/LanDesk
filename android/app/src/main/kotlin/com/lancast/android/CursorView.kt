package com.lancast.android

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.View
import com.lancast.core.CursorShape

// Cursor del PC dibujado en el teléfono (el video no lo incluye). Se mueve al instante con el dedo.
class CursorView(context: Context) : View(context) {
    private var bitmap: Bitmap? = null
    private var hotX = 0
    private var hotY = 0
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val dst = RectF()
    private val fallback = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFFFFFF.toInt() }
    private val outline = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFF000000.toInt()
        style = Paint.Style.STROKE
        strokeWidth = 2f
    }

    var px = 0f  // Posición en pantalla (píxeles de la vista).
    var py = 0f
    var scale = 1f  // Píxeles de pantalla por píxel del stream.
    var shown = false

    fun setShape(s: CursorShape) {
        bitmap = toBitmap(s)
        hotX = s.hotX
        hotY = s.hotY
        invalidate()
    }

    fun moveTo(x: Float, y: Float) {
        px = x
        py = y
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        if (!shown) return
        val k = scale.coerceIn(0.75f, 3f)
        val b = bitmap
        if (b == null) {
            canvas.drawCircle(px, py, 8f, fallback)
            canvas.drawCircle(px, py, 8f, outline)
            return
        }
        val left = px - hotX * k
        val top = py - hotY * k
        dst.set(left, top, left + b.width * k, top + b.height * k)
        canvas.drawBitmap(b, null, dst, paint)
    }

    companion object {
        // Formatos de DXGI: 1 = monocromo (máscara AND y luego XOR, 1 bit), 2 = color BGRA, 4 = color con máscara.
        fun toBitmap(s: CursorShape): Bitmap? {
            if (s.width <= 0 || s.height <= 0) return null
            return when (s.type) {
                2, 4 -> {
                    val px = IntArray(s.width * s.height)
                    for (y in 0 until s.height) for (x in 0 until s.width) {
                        val o = y * s.pitch + x * 4
                        val b = s.data[o].toInt() and 0xFF
                        val g = s.data[o + 1].toInt() and 0xFF
                        val r = s.data[o + 2].toInt() and 0xFF
                        var a = s.data[o + 3].toInt() and 0xFF
                        // Con máscara: alfa 0 = color opaco; 0xFF = XOR con la pantalla (se dibuja solo si no es negro).
                        if (s.type == 4) a = if (a == 0) 0xFF else if (r or g or b != 0) 0xFF else 0
                        px[y * s.width + x] = (a shl 24) or (r shl 16) or (g shl 8) or b
                    }
                    Bitmap.createBitmap(px, s.width, s.height, Bitmap.Config.ARGB_8888)
                }
                1 -> {
                    val h = s.height / 2
                    val px = IntArray(s.width * h)
                    for (y in 0 until h) for (x in 0 until s.width) {
                        val bit = 0x80 ushr (x % 8)
                        val and = (s.data[y * s.pitch + x / 8].toInt() and bit) != 0
                        val xor = (s.data[(y + h) * s.pitch + x / 8].toInt() and bit) != 0
                        px[y * s.width + x] = when {
                            !and && !xor -> 0xFF000000.toInt()
                            !and && xor -> 0xFFFFFFFF.toInt()
                            and && xor -> 0xFF000000.toInt()  // Invertir: se aproxima con negro.
                            else -> 0
                        }
                    }
                    Bitmap.createBitmap(px, s.width, h, Bitmap.Config.ARGB_8888)
                }
                else -> null
            }
        }
    }
}
