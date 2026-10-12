package com.lancast.android

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import com.lancast.core.GamepadState
import com.lancast.core.Messages
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.roundToInt

private fun axis(v: Float) = (v.coerceIn(-1f, 1f) * 32767f).roundToInt()
private fun trigger(v: Float) = (v.coerceIn(0f, 1f) * 255f).roundToInt()

// Mandos físicos conectados al teléfono (Bluetooth o USB) -> mandos de Xbox en el PC (índices 0..2; el 3 es el
// mando en pantalla).
class GamepadInput(private val send: (ByteArray) -> Unit) {
    private val slots = HashMap<Int, Int>()  // deviceId -> índice
    private val states = HashMap<Int, GamepadState>()

    private fun slot(deviceId: Int): Int? = slots[deviceId] ?: (0..2).firstOrNull { it !in slots.values }?.also { slots[deviceId] = it }

    private fun publish(deviceId: Int, next: GamepadState) {
        if (states[deviceId] == next) return
        states[deviceId] = next
        send(Messages.gamepad(next))
    }

    private fun current(deviceId: Int): GamepadState? {
        val i = slot(deviceId) ?: return null
        return states[deviceId] ?: GamepadState(index = i)
    }

    fun onKey(event: KeyEvent): Boolean {
        if (!isGamepad(event.source)) return false
        val bit = when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_A -> GamepadState.A
            KeyEvent.KEYCODE_BUTTON_B -> GamepadState.B
            KeyEvent.KEYCODE_BUTTON_X -> GamepadState.X
            KeyEvent.KEYCODE_BUTTON_Y -> GamepadState.Y
            KeyEvent.KEYCODE_BUTTON_L1 -> GamepadState.LEFT_SHOULDER
            KeyEvent.KEYCODE_BUTTON_R1 -> GamepadState.RIGHT_SHOULDER
            KeyEvent.KEYCODE_BUTTON_THUMBL -> GamepadState.LEFT_THUMB
            KeyEvent.KEYCODE_BUTTON_THUMBR -> GamepadState.RIGHT_THUMB
            KeyEvent.KEYCODE_BUTTON_START -> GamepadState.START
            KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BACK -> GamepadState.BACK
            KeyEvent.KEYCODE_BUTTON_MODE -> GamepadState.GUIDE
            KeyEvent.KEYCODE_DPAD_UP -> GamepadState.DPAD_UP
            KeyEvent.KEYCODE_DPAD_DOWN -> GamepadState.DPAD_DOWN
            KeyEvent.KEYCODE_DPAD_LEFT -> GamepadState.DPAD_LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT -> GamepadState.DPAD_RIGHT
            KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2 -> 0
            else -> return false
        }
        if (event.repeatCount > 0) return true
        val s = current(event.deviceId) ?: return true
        val down = event.action == KeyEvent.ACTION_DOWN
        var next = if (bit != 0) s.copy(buttons = if (down) s.buttons or bit else s.buttons and bit.inv()) else s
        // Gatillos digitales (mandos sin eje analógico).
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_L2) next = next.copy(leftTrigger = if (down) 255 else 0)
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_R2) next = next.copy(rightTrigger = if (down) 255 else 0)
        publish(event.deviceId, next)
        return true
    }

    fun onMotion(event: MotionEvent): Boolean {
        if (!isGamepad(event.source) || event.action != MotionEvent.ACTION_MOVE) return false
        val s = current(event.deviceId) ?: return true
        val dev = event.device
        fun v(a: Int): Float {
            val raw = event.getAxisValue(a)
            val flat = dev?.getMotionRange(a, event.source)?.flat ?: 0f
            return if (abs(raw) <= flat) 0f else raw
        }
        val rx = if (dev?.getMotionRange(MotionEvent.AXIS_Z) != null) v(MotionEvent.AXIS_Z) else v(MotionEvent.AXIS_RX)
        val ry = if (dev?.getMotionRange(MotionEvent.AXIS_RZ) != null) v(MotionEvent.AXIS_RZ) else v(MotionEvent.AXIS_RY)
        val lt = maxOf(v(MotionEvent.AXIS_LTRIGGER), v(MotionEvent.AXIS_BRAKE))
        val rt = maxOf(v(MotionEvent.AXIS_RTRIGGER), v(MotionEvent.AXIS_GAS))
        val hatX = v(MotionEvent.AXIS_HAT_X)
        val hatY = v(MotionEvent.AXIS_HAT_Y)
        var buttons = s.buttons and (GamepadState.DPAD_UP or GamepadState.DPAD_DOWN or GamepadState.DPAD_LEFT or GamepadState.DPAD_RIGHT).inv()
        if (dev?.getMotionRange(MotionEvent.AXIS_HAT_X) != null) {
            if (hatX < -0.5f) buttons = buttons or GamepadState.DPAD_LEFT
            if (hatX > 0.5f) buttons = buttons or GamepadState.DPAD_RIGHT
            if (hatY < -0.5f) buttons = buttons or GamepadState.DPAD_UP
            if (hatY > 0.5f) buttons = buttons or GamepadState.DPAD_DOWN
        } else {
            buttons = s.buttons
        }
        // En Android el eje Y crece hacia abajo; en XInput, hacia arriba.
        publish(event.deviceId, s.copy(
            buttons = buttons,
            leftX = axis(v(MotionEvent.AXIS_X)), leftY = axis(-v(MotionEvent.AXIS_Y)),
            rightX = axis(rx), rightY = axis(-ry),
            leftTrigger = trigger(lt), rightTrigger = trigger(rt),
        ))
        return true
    }

    // Al salir del stream: los mandos se quitan del PC.
    fun disconnectAll() {
        for ((_, s) in states) send(Messages.gamepad(s.copy(connected = false)))
        states.clear()
        slots.clear()
    }

    companion object {
        fun isGamepad(source: Int) = source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
    }
}

// Mando de Xbox dibujado en la pantalla (multitáctil): sticks, cruceta, A/B/X/Y, bumpers, gatillos, Back/Start.
class OnScreenPad(context: Context, private val send: (ByteArray) -> Unit) : View(context) {
    private sealed class Control {
        data class Button(val bit: Int, val label: String, val cx: Float, val cy: Float, val r: Float) : Control()
        data class Trigger(val left: Boolean, val label: String, val cx: Float, val cy: Float, val r: Float) : Control()
        data class Stick(val left: Boolean, val cx: Float, val cy: Float, val r: Float) : Control()
    }

    private val density = resources.displayMetrics.density
    private var controls: List<Control> = emptyList()
    private val owner = HashMap<Int, Control>()  // pointerId -> control
    private val stickPos = HashMap<Boolean, Pair<Float, Float>>()
    private var state = GamepadState(index = 3)
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x40FFFFFF }
    private val pressed = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xA04A90E2.toInt() }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = 0x80FFFFFF.toInt()
        strokeWidth = 1.5f * density
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xE0FFFFFF.toInt()
        textAlign = Paint.Align.CENTER
        textSize = 15 * density
        isFakeBoldText = true
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        val u = minOf(w, h) / 10f  // Unidad: una décima del lado corto.
        val br = u * 0.55f
        val W = w.toFloat()
        val H = h.toFloat()
        controls = listOf(
            Control.Stick(true, u * 2.0f, H - u * 2.0f, u * 1.3f),
            Control.Button(GamepadState.DPAD_UP, "▲", u * 4.2f, H - u * 4.0f, br * 0.8f),
            Control.Button(GamepadState.DPAD_DOWN, "▼", u * 4.2f, H - u * 2.4f, br * 0.8f),
            Control.Button(GamepadState.DPAD_LEFT, "◀", u * 3.4f, H - u * 3.2f, br * 0.8f),
            Control.Button(GamepadState.DPAD_RIGHT, "▶", u * 5.0f, H - u * 3.2f, br * 0.8f),
            Control.Button(GamepadState.A, "A", W - u * 2.0f, H - u * 1.2f, br),
            Control.Button(GamepadState.B, "B", W - u * 1.0f, H - u * 2.2f, br),
            Control.Button(GamepadState.X, "X", W - u * 3.0f, H - u * 2.2f, br),
            Control.Button(GamepadState.Y, "Y", W - u * 2.0f, H - u * 3.2f, br),
            Control.Stick(false, W - u * 4.8f, H - u * 1.6f, u * 1.1f),
            Control.Trigger(true, "LT", u * 1.2f, u * 1.2f, br),
            Control.Button(GamepadState.LEFT_SHOULDER, "LB", u * 1.2f, u * 2.6f, br),
            Control.Trigger(false, "RT", W - u * 1.2f, u * 1.2f, br),
            Control.Button(GamepadState.RIGHT_SHOULDER, "RB", W - u * 1.2f, u * 2.6f, br),
            Control.Button(GamepadState.BACK, "⧉", W / 2 - u, u * 1.0f, br * 0.7f),
            Control.Button(GamepadState.START, "≡", W / 2 + u, u * 1.0f, br * 0.7f),
        )
    }

    private fun hit(x: Float, y: Float): Control? = controls.firstOrNull { c ->
        val (cx, cy, r) = when (c) {
            is Control.Button -> Triple(c.cx, c.cy, c.r)
            is Control.Trigger -> Triple(c.cx, c.cy, c.r)
            is Control.Stick -> Triple(c.cx, c.cy, c.r)
        }
        hypot(x - cx, y - cy) <= r * 1.35f
    }

    @Suppress("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        val i = e.actionIndex
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val c = hit(e.getX(i), e.getY(i)) ?: return owner.isNotEmpty()
                owner[e.getPointerId(i)] = c
                if (c is Control.Stick) stickPos[c.left] = e.getX(i) to e.getY(i)
                performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY)
            }
            MotionEvent.ACTION_MOVE -> for (p in 0 until e.pointerCount) {
                val c = owner[e.getPointerId(p)]
                if (c is Control.Stick) stickPos[c.left] = e.getX(p) to e.getY(p)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> owner.remove(e.getPointerId(i))?.let { if (it is Control.Stick) stickPos.remove(it.left) }
            MotionEvent.ACTION_CANCEL -> {
                owner.clear()
                stickPos.clear()
            }
        }
        recompute()
        return true
    }

    private fun recompute() {
        var buttons = 0
        var lt = 0
        var rt = 0
        var l = 0 to 0
        var r = 0 to 0
        for (c in owner.values) when (c) {
            is Control.Button -> buttons = buttons or c.bit
            is Control.Trigger -> if (c.left) lt = 255 else rt = 255
            is Control.Stick -> {
                val (x, y) = stickPos[c.left] ?: continue
                var dx = (x - c.cx) / c.r
                var dy = (y - c.cy) / c.r
                val len = hypot(dx, dy)
                if (len > 1f) {
                    dx /= len
                    dy /= len
                }
                val v = axis(dx) to axis(-dy)
                if (c.left) l = v else r = v
            }
        }
        val next = GamepadState(3, true, buttons, lt, rt, l.first, l.second, r.first, r.second)
        if (next != state) {
            state = next
            send(Messages.gamepad(next))
            invalidate()
        }
    }

    fun release() {
        owner.clear()
        stickPos.clear()
        send(Messages.gamepad(GamepadState(index = 3, connected = false)))
        state = GamepadState(index = 3)
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        for (c in controls) when (c) {
            is Control.Button -> {
                canvas.drawCircle(c.cx, c.cy, c.r, if (state.buttons and c.bit != 0) pressed else fill)
                canvas.drawCircle(c.cx, c.cy, c.r, stroke)
                canvas.drawText(c.label, c.cx, c.cy + text.textSize / 3, text)
            }
            is Control.Trigger -> {
                val on = if (c.left) state.leftTrigger > 0 else state.rightTrigger > 0
                canvas.drawCircle(c.cx, c.cy, c.r, if (on) pressed else fill)
                canvas.drawCircle(c.cx, c.cy, c.r, stroke)
                canvas.drawText(c.label, c.cx, c.cy + text.textSize / 3, text)
            }
            is Control.Stick -> {
                canvas.drawCircle(c.cx, c.cy, c.r, fill)
                canvas.drawCircle(c.cx, c.cy, c.r, stroke)
                val x = (if (c.left) state.leftX else state.rightX) / 32767f
                val y = -(if (c.left) state.leftY else state.rightY) / 32767f
                canvas.drawCircle(c.cx + x * c.r, c.cy + y * c.r, c.r * 0.45f, pressed)
            }
        }
    }
}
