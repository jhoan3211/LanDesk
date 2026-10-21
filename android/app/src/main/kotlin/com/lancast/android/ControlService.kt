package com.lancast.android

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.content.Context
import android.graphics.Path
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.DisplayMetrics
import android.view.WindowManager
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import com.lancast.core.RemoteInput
import kotlin.math.abs

// Control remoto del teléfono: Android no deja que otra app toque la pantalla salvo un servicio de accesibilidad
// que el usuario activa en Ajustes. Mouse -> toques y deslizamientos; rueda -> desplazamiento; clic derecho ->
// Atrás; teclado y texto -> el campo de texto enfocado.
class ControlService : AccessibilityService() {
    private val main = Handler(Looper.getMainLooper())

    // Cursor en píxeles de la pantalla real.
    private var cx = 0f
    private var cy = 0f
    private var down = false
    private var stroke: GestureDescription.StrokeDescription? = null
    private var inFlight = false
    private val pending = ArrayList<Pair<Float, Float>>()
    private var lastX = 0f
    private var lastY = 0f
    private var ctrl = false
    private var shift = false
    private var alt = false

    override fun onServiceConnected() {
        instance = this
    }

    override fun onUnbind(intent: android.content.Intent?): Boolean {
        instance = null
        return super.onUnbind(intent)
    }

    override fun onDestroy() {
        instance = null
        super.onDestroy()
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {}
    override fun onInterrupt() {}

    // Desde la red (cualquier hilo).
    fun handle(input: RemoteInput) = main.post { apply(input) }.let {}

    fun releaseAll() = main.post {
        if (down) up()
        ctrl = false
        shift = false
        alt = false
    }.let {}

    private fun screen(): Pair<Int, Int> {
        val m = DisplayMetrics()
        @Suppress("DEPRECATION")
        (getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.getRealMetrics(m)
        return m.widthPixels to m.heightPixels
    }

    private fun apply(input: RemoteInput) {
        when (input) {
            is RemoteInput.MouseAbs -> {
                val (w, h) = screen()
                cx = (input.x.coerceIn(0f, 1f) * (w - 1))
                cy = (input.y.coerceIn(0f, 1f) * (h - 1))
                if (down) {
                    pending += cx to cy
                    if (!inFlight) flush()
                }
            }
            is RemoteInput.MouseButton -> when (input.button) {
                0 -> if (input.down) press() else if (down) up()
                1 -> if (input.down) performGlobalAction(GLOBAL_ACTION_BACK)  // Clic derecho = Atrás.
                2 -> if (input.down) performGlobalAction(GLOBAL_ACTION_HOME)
                3 -> if (input.down) performGlobalAction(GLOBAL_ACTION_BACK)
                4 -> if (input.down) performGlobalAction(GLOBAL_ACTION_RECENTS)
            }
            is RemoteInput.Wheel -> scroll(input.delta, input.horizontal)
            is RemoteInput.Text -> insertText(input.text)
            is RemoteInput.Key -> key(input.scancode, input.extended, input.down)
            is RemoteInput.ReleaseAll -> {
                if (down) up()
                ctrl = false
                shift = false
                alt = false
            }
            is RemoteInput.Clipboard -> {}
        }
    }

    // --------------------------------------------------------------------------------------- toques

    // El dedo se apoya y queda abajo (trazo que continúa) hasta que se suelta el botón: así funcionan tocar, mantener
    // y arrastrar en vivo.
    private fun press() {
        if (down) return
        down = true
        lastX = cx
        lastY = cy
        pending.clear()
        val path = Path().apply {
            moveTo(cx, cy)
            lineTo(cx, cy)
        }
        val s = GestureDescription.StrokeDescription(path, 0, 1, true)
        dispatch(s)
    }

    private fun up() {
        down = false
        if (!inFlight) flush()
    }

    private fun flush() {
        val prev = stroke ?: return
        if (!down && pending.isEmpty() && !prev.willContinue()) return
        val path = Path().apply { moveTo(lastX, lastY) }
        if (pending.isEmpty()) {
            path.lineTo(lastX, lastY)
        } else {
            for ((x, y) in pending) path.lineTo(x, y)
            lastX = pending.last().first
            lastY = pending.last().second
        }
        val duration = (pending.size * 8L).coerceIn(1, 60)
        pending.clear()
        dispatch(prev.continueStroke(path, 0, duration, down))
    }

    private fun dispatch(s: GestureDescription.StrokeDescription) {
        stroke = s
        inFlight = true
        val ok = dispatchGesture(GestureDescription.Builder().addStroke(s).build(), object : GestureResultCallback() {
            override fun onCompleted(gestureDescription: GestureDescription?) {
                inFlight = false
                if (pending.isNotEmpty() || (!down && stroke?.willContinue() == true)) flush()
                else if (!down) stroke = null
            }

            override fun onCancelled(gestureDescription: GestureDescription?) {
                inFlight = false
                stroke = null
                pending.clear()
            }
        }, main)
        if (!ok) {
            inFlight = false
            stroke = null
        }
    }

    // Rueda: un deslizamiento corto en el lugar del cursor (hacia arriba = contenido baja, como en el PC).
    private fun scroll(delta: Int, horizontal: Boolean) {
        if (down) return
        val (w, h) = screen()
        val dist = (abs(delta) / 120f).coerceIn(0.25f, 6f) * minOf(w, h) * 0.12f
        val sign = if (delta > 0) 1f else -1f
        val path = Path().apply {
            moveTo(cx, cy)
            if (horizontal) lineTo((cx - sign * dist).coerceIn(0f, w - 1f), cy)
            else lineTo(cx, (cy + sign * dist).coerceIn(0f, h - 1f))
        }
        dispatchGesture(GestureDescription.Builder().addStroke(GestureDescription.StrokeDescription(path, 0, 120)).build(), null, null)
    }

    // --------------------------------------------------------------------------------------- teclado y texto

    private fun focused(): AccessibilityNodeInfo? =
        rootInActiveWindow?.findFocus(AccessibilityNodeInfo.FOCUS_INPUT)?.takeIf { it.isEditable }

    private fun currentText(node: AccessibilityNodeInfo): String {
        if (node.isShowingHintText) return ""
        return node.text?.toString() ?: ""
    }

    private fun setText(node: AccessibilityNodeInfo, text: String, cursor: Int) {
        node.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, Bundle().apply {
            putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, text)
        })
        select(node, cursor, cursor)
    }

    private fun select(node: AccessibilityNodeInfo, start: Int, end: Int) {
        node.performAction(AccessibilityNodeInfo.ACTION_SET_SELECTION, Bundle().apply {
            putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, start)
            putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, end)
        })
    }

    private fun selection(node: AccessibilityNodeInfo, len: Int): Pair<Int, Int> {
        var s = node.textSelectionStart
        var e = node.textSelectionEnd
        if (s < 0 || e < 0) {
            s = len
            e = len
        }
        return minOf(s, e).coerceIn(0, len) to maxOf(s, e).coerceIn(0, len)
    }

    private fun insertText(t: String) {
        val node = focused() ?: return
        val text = currentText(node)
        val (s, e) = selection(node, text.length)
        setText(node, text.substring(0, s) + t + text.substring(e), s + t.length)
    }

    private fun key(sc: Int, ext: Boolean, isDown: Boolean) {
        when (sc) {
            0x1D -> { ctrl = isDown; return }
            0x2A, 0x36 -> { shift = isDown; return }
            0x38 -> { alt = isDown; return }
        }
        if (!isDown) return
        if (ctrl) {
            val node = focused()
            when (sc) {
                0x1E -> node?.let { select(it, 0, currentText(it).length) }  // Ctrl+A
                0x2E -> node?.performAction(AccessibilityNodeInfo.ACTION_COPY)  // Ctrl+C
                0x2F -> node?.performAction(AccessibilityNodeInfo.ACTION_PASTE)  // Ctrl+V
                0x2D -> node?.performAction(AccessibilityNodeInfo.ACTION_CUT)  // Ctrl+X
            }
            return
        }
        if (alt && sc == 0x0F) {
            performGlobalAction(GLOBAL_ACTION_RECENTS)  // Alt+Tab
            return
        }
        when {
            sc == 0x01 -> performGlobalAction(GLOBAL_ACTION_BACK)  // Esc
            ext && (sc == 0x5B || sc == 0x5C) -> performGlobalAction(GLOBAL_ACTION_HOME)  // Windows
            sc == 0x0E -> edit { text, s, e -> if (s != e) Triple(text.removeRange(s, e), s, s) else if (s > 0) Triple(text.removeRange(s - 1, s), s - 1, s - 1) else null }
            ext && sc == 0x53 -> edit { text, s, e -> if (s != e) Triple(text.removeRange(s, e), s, s) else if (s < text.length) Triple(text.removeRange(s, s + 1), s, s) else null }
            sc == 0x1C -> enter()
            ext && sc == 0x4B -> move(-1)
            ext && sc == 0x4D -> move(1)
            ext && sc == 0x47 -> focused()?.let { select(it, 0, 0) }
            ext && sc == 0x4F -> focused()?.let { val l = currentText(it).length; select(it, l, l) }
            else -> Scancodes.charFor(sc, shift)?.let { insertText(it.toString()) }
        }
    }

    private inline fun edit(change: (String, Int, Int) -> Triple<String, Int, Int>?) {
        val node = focused() ?: return
        val text = currentText(node)
        val (s, e) = selection(node, text.length)
        val r = change(text, s, e) ?: return
        setText(node, r.first, r.second)
    }

    private fun move(dir: Int) {
        val node = focused() ?: return
        val len = currentText(node).length
        val (s, e) = selection(node, len)
        val p = (if (dir < 0) s - 1 else e + 1).coerceIn(0, len)
        select(node, p, p)
    }

    private fun enter() {
        val node = focused()
        if (node == null) {
            // Sin campo de texto: Enter toca donde está el cursor.
            press()
            up()
            return
        }
        if (Build.VERSION.SDK_INT >= 30 && node.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_IME_ENTER.id)) return
        insertText("\n")
    }

    companion object {
        @Volatile var instance: ControlService? = null
            private set

        fun enabled() = instance != null
    }
}
