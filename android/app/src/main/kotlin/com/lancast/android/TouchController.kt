package com.lancast.android

import android.graphics.RectF
import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import com.lancast.core.Messages
import com.lancast.core.MouseButtonId
import kotlin.math.abs
import kotlin.math.hypot

// Gestos táctiles -> mouse del PC.
//
// Touchpad (predeterminado): el dedo mueve el cursor como un touchpad de laptop.
//   toque = clic · toque con 2 dedos = clic derecho · arrastrar con 2 dedos = rueda · pellizcar = zoom
//   mantener presionado (o doble toque y arrastrar) = arrastrar con el botón izquierdo · toque con 3 dedos = teclado
// Directo: el toque cae donde está el dedo (clic, arrastrar, 2 dedos = rueda).
// En los dos modos: arrastrar con 3 dedos mueve la vista ampliada (zoom).
class TouchController(private val host: Host, private val density: Float) {
    interface Host {
        fun videoRect(): RectF      // Dónde se ve el stream en la pantalla (con el zoom aplicado).
        fun zoomed(): Boolean
        fun send(message: ByteArray)
        fun cursorMoved(nx: Float, ny: Float)
        fun zoomBy(factor: Float, focusX: Float, focusY: Float)
        fun panBy(dx: Float, dy: Float)
        fun toggleKeyboard()
        fun haptic()
    }

    var touchpad = true
    var cx = 0.5f  // Cursor normalizado en el stream.
        private set
    var cy = 0.5f
        private set
    var touching = false
        private set

    private val handler = Handler(Looper.getMainLooper())
    private val slop = 8 * density
    private var downX = 0f
    private var downY = 0f
    private var downTime = 0L
    private var lastX = 0f
    private var lastY = 0f
    private var lastMoveTime = 0L
    private var moved = false
    private var maxPointers = 0
    private var dragging = false
    private var lastTapTime = 0L
    private var lastTapX = 0f
    private var lastTapY = 0f
    private var pendingDrag = false

    private enum class Two { NONE, SCROLL, ZOOM }
    private var two = Two.NONE
    private var twoStartDist = 0f
    private var twoLastDist = 0f
    private var twoStartX = 0f
    private var twoStartY = 0f
    private var twoLastX = 0f
    private var twoLastY = 0f
    private var scrollAccumY = 0f
    private var scrollAccumX = 0f
    private var threeLastX = 0f
    private var threeLastY = 0f
    private var threeMoved = false

    private val longPress = Runnable {
        if (touching && !moved && maxPointers == 1 && !dragging) {
            startDrag()
            host.haptic()
        }
    }

    // Sincroniza con la posición real del cursor del PC (cuando no se está tocando).
    fun syncCursor(nx: Float, ny: Float) {
        if (touching) return
        cx = nx.coerceIn(0f, 1f)
        cy = ny.coerceIn(0f, 1f)
    }

    fun onTouch(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                touching = true
                downX = e.x
                downY = e.y
                lastX = e.x
                lastY = e.y
                downTime = e.eventTime
                lastMoveTime = e.eventTime
                moved = false
                maxPointers = 1
                two = Two.NONE
                pendingDrag = touchpad && e.eventTime - lastTapTime < 300 && hypot(e.x - lastTapX, e.y - lastTapY) < 40 * density
                handler.postDelayed(longPress, 450)
                if (!touchpad) moveAbsoluteTo(e.x, e.y)
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                maxPointers = maxOf(maxPointers, e.pointerCount)
                handler.removeCallbacks(longPress)
                if (e.pointerCount == 3) {
                    // Tres dedos: mover la vista con zoom (o, si no se mueven, abrir el teclado al soltar).
                    threeLastX = centroid3X(e)
                    threeLastY = centroid3Y(e)
                    threeMoved = false
                }
                if (e.pointerCount == 2) {
                    if (dragging) endDrag()
                    two = Two.NONE
                    twoStartDist = distance(e)
                    twoLastDist = twoStartDist
                    twoStartX = centroidX(e)
                    twoStartY = centroidY(e)
                    twoLastX = twoStartX
                    twoLastY = twoStartY
                    scrollAccumX = 0f
                    scrollAccumY = 0f
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (e.pointerCount >= 3) {
                    threeFingerMove(e)
                } else if (e.pointerCount >= 2 && maxPointers == 2) {
                    twoFingerMove(e)
                } else if (e.pointerCount == 1 && maxPointers == 1) {
                    oneFingerMove(e)
                }
            }
            MotionEvent.ACTION_POINTER_UP -> {
                // Al soltar un dedo de dos, el que queda no debe mover el cursor de golpe.
                val keep = if (e.actionIndex == 0) 1 else 0
                lastX = e.getX(keep)
                lastY = e.getY(keep)
            }
            MotionEvent.ACTION_UP -> {
                handler.removeCallbacks(longPress)
                val duration = e.eventTime - downTime
                when {
                    dragging -> endDrag()
                    maxPointers == 1 && !moved && duration < 300 -> {
                        if (!touchpad) moveAbsoluteTo(e.x, e.y)
                        click(MouseButtonId.LEFT)
                        lastTapTime = e.eventTime
                        lastTapX = e.x
                        lastTapY = e.y
                    }
                    maxPointers == 2 && two == Two.NONE && duration < 350 -> click(MouseButtonId.RIGHT)
                    maxPointers == 3 && !threeMoved && duration < 450 -> host.toggleKeyboard()
                }
                touching = false
                pendingDrag = false
            }
            MotionEvent.ACTION_CANCEL -> {
                handler.removeCallbacks(longPress)
                if (dragging) endDrag()
                touching = false
            }
        }
        return true
    }

    private fun oneFingerMove(e: MotionEvent) {
        val dx = e.x - lastX
        val dy = e.y - lastY
        if (!moved && hypot(e.x - downX, e.y - downY) > slop) {
            moved = true
            handler.removeCallbacks(longPress)
            if (pendingDrag) startDrag()
            if (!touchpad && !dragging) {
                // Directo: arrastrar desde donde empezó el dedo (seleccionar, mover ventanas).
                moveAbsoluteTo(downX, downY)
                startDrag()
            }
        }
        if (!moved) return
        if (touchpad) {
            val rect = host.videoRect()
            val dt = (e.eventTime - lastMoveTime).coerceAtLeast(1)
            val speed = hypot(dx, dy) / dt * 1000f / density  // dp por segundo
            val gain = 1.0f + (speed / 900f).coerceAtMost(1.6f)  // Aceleración suave, como un touchpad.
            cx = (cx + dx / rect.width() * gain).coerceIn(0f, 1f)
            cy = (cy + dy / rect.height() * gain).coerceIn(0f, 1f)
            sendCursor()
        } else {
            moveAbsoluteTo(e.x, e.y)
        }
        lastX = e.x
        lastY = e.y
        lastMoveTime = e.eventTime
    }

    private fun twoFingerMove(e: MotionEvent) {
        val dist = distance(e)
        val x = centroidX(e)
        val y = centroidY(e)
        if (two == Two.NONE) {
            if (abs(dist - twoStartDist) > 36 * density) {
                two = Two.ZOOM
            } else if (hypot(x - twoStartX, y - twoStartY) > 14 * density) {
                two = Two.SCROLL
            }
        }
        when (two) {
            Two.ZOOM -> if (twoLastDist > 0) host.zoomBy(dist / twoLastDist, x, y)
            Two.SCROLL -> if (!touchpad && host.zoomed()) {
                host.panBy(x - twoLastX, y - twoLastY)
            } else {
                // Rueda: 120 = un "clic" de rueda; se envían valores finos para que el desplazamiento sea suave.
                scrollAccumY += (y - twoLastY) / density
                scrollAccumX += (x - twoLastX) / density
                val vy = (scrollAccumY * 4f).toInt()
                if (abs(vy) >= 12) {
                    host.send(Messages.mouseWheel(vy))
                    scrollAccumY -= vy / 4f
                }
                val vx = (scrollAccumX * 4f).toInt()
                if (abs(vx) >= 12) {
                    host.send(Messages.mouseWheel(-vx, horizontal = true))
                    scrollAccumX -= vx / 4f
                }
            }
            Two.NONE -> {}
        }
        twoLastDist = dist
        twoLastX = x
        twoLastY = y
    }

    private fun threeFingerMove(e: MotionEvent) {
        val x = centroid3X(e)
        val y = centroid3Y(e)
        val dx = x - threeLastX
        val dy = y - threeLastY
        if (!threeMoved && hypot(dx, dy) > slop) threeMoved = true
        if (threeMoved) {
            host.panBy(dx, dy)
            threeLastX = x
            threeLastY = y
        }
    }

    private fun centroid3X(e: MotionEvent) = (e.getX(0) + e.getX(1) + e.getX(2)) / 3
    private fun centroid3Y(e: MotionEvent) = (e.getY(0) + e.getY(1) + e.getY(2)) / 3

    private fun moveAbsoluteTo(x: Float, y: Float) {
        val r = host.videoRect()
        cx = ((x - r.left) / r.width()).coerceIn(0f, 1f)
        cy = ((y - r.top) / r.height()).coerceIn(0f, 1f)
        sendCursor()
    }

    private fun sendCursor() {
        host.send(Messages.mouseAbs(cx, cy))
        host.cursorMoved(cx, cy)
    }

    private fun click(button: MouseButtonId) {
        host.send(Messages.mouseAbs(cx, cy))
        host.send(Messages.mouseButton(button, true))
        host.send(Messages.mouseButton(button, false))
    }

    private fun startDrag() {
        dragging = true
        host.send(Messages.mouseButton(MouseButtonId.LEFT, true))
    }

    private fun endDrag() {
        dragging = false
        host.send(Messages.mouseButton(MouseButtonId.LEFT, false))
    }

    private fun distance(e: MotionEvent) = if (e.pointerCount < 2) 0f else hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1))
    private fun centroidX(e: MotionEvent) = if (e.pointerCount < 2) e.x else (e.getX(0) + e.getX(1)) / 2
    private fun centroidY(e: MotionEvent) = if (e.pointerCount < 2) e.y else (e.getY(0) + e.getY(1)) / 2
}
