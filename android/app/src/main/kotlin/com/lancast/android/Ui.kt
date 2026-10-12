package com.lancast.android

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

// Pequeños ayudantes para armar la interfaz en código (sin dependencias extra).
object Ui {
    const val BG = 0xFF13151A.toInt()
    const val PANEL = 0xFF1C1F26.toInt()
    const val ACCENT = 0xFF4A90E2.toInt()
    const val TEXT = 0xFFE6E6EB.toInt()
    const val MUTED = 0xFF8C919B.toInt()
    const val DANGER = 0xFFD9534F.toInt()
    const val GOOD = 0xFF4CC777.toInt()

    fun Context.dp(v: Float): Int = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, resources.displayMetrics).toInt()
    fun Context.dp(v: Int): Int = dp(v.toFloat())

    fun rounded(color: Int, radius: Float, stroke: Int = 0, strokeColor: Int = 0) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = radius
        if (stroke > 0) setStroke(stroke, strokeColor)
    }

    fun Context.text(value: String, size: Float = 15f, color: Int = TEXT, bold: Boolean = false) = TextView(this).apply {
        text = value
        textSize = size
        setTextColor(color)
        if (bold) typeface = Typeface.DEFAULT_BOLD
    }

    fun Context.button(label: String, primary: Boolean = false, danger: Boolean = false, onClick: () -> Unit) = Button(this).apply {
        text = label
        isAllCaps = false
        textSize = 15f
        setTextColor(Color.WHITE)
        background = rounded(if (danger) DANGER else if (primary) ACCENT else 0xFF2A2E37.toInt(), dp(10).toFloat())
        setPadding(dp(16), dp(10), dp(16), dp(10))
        minHeight = 0
        minimumHeight = 0
        setOnClickListener { onClick() }
    }

    fun Context.column(padding: Int = 0) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(padding), dp(padding), dp(padding), dp(padding))
    }

    fun Context.row() = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
    }

    fun lp(w: Int = ViewGroup.LayoutParams.MATCH_PARENT, h: Int = ViewGroup.LayoutParams.WRAP_CONTENT, weight: Float = 0f) =
        LinearLayout.LayoutParams(w, h, weight)

    fun View.margins(l: Int = 0, t: Int = 0, r: Int = 0, b: Int = 0): View {
        val p = (layoutParams as? ViewGroup.MarginLayoutParams) ?: lp()
        p.setMargins(l, t, r, b)
        layoutParams = p
        return this
    }
}
