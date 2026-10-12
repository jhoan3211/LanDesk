package com.lancast.android

import android.content.Context
import android.text.InputType
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import com.lancast.core.Messages

// Teclas del PC por scancode (conjunto 1, como las manda el cliente de Windows).
object Scancodes {
    data class K(val code: Int, val extended: Boolean = false)

    val ESC = K(0x01)
    val BACKSPACE = K(0x0E)
    val TAB = K(0x0F)
    val ENTER = K(0x1C)
    val CTRL = K(0x1D)
    val SHIFT = K(0x2A)
    val ALT = K(0x38)
    val SPACE = K(0x39)
    val WIN = K(0x5B, true)
    val HOME = K(0x47, true)
    val UP = K(0x48, true)
    val PAGE_UP = K(0x49, true)
    val LEFT = K(0x4B, true)
    val RIGHT = K(0x4D, true)
    val END = K(0x4F, true)
    val DOWN = K(0x50, true)
    val PAGE_DOWN = K(0x51, true)
    val DELETE = K(0x53, true)
    fun f(n: Int) = if (n <= 10) K(0x3A + n) else K(0x57 + n - 11)

    private val letters = "qwertyuiop" to 0x10
    private val letters2 = "asdfghjkl" to 0x1E
    private val letters3 = "zxcvbnm" to 0x2C

    // Letras y números (distribución de EE. UU.) para combinaciones como Ctrl+C.
    fun forChar(c: Char): K? {
        val l = c.lowercaseChar()
        for ((row, base) in listOf(letters, letters2, letters3)) {
            val i = row.indexOf(l)
            if (i >= 0) return K(base + i)
        }
        if (l in '1'..'9') return K(0x02 + (l - '1'))
        if (l == '0') return K(0x0B)
        if (l == ' ') return SPACE
        return null
    }

    fun forKeyCode(code: Int): K? = when (code) {
        KeyEvent.KEYCODE_DEL -> BACKSPACE
        KeyEvent.KEYCODE_FORWARD_DEL -> DELETE
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> ENTER
        KeyEvent.KEYCODE_TAB -> TAB
        KeyEvent.KEYCODE_ESCAPE -> ESC
        KeyEvent.KEYCODE_DPAD_LEFT -> LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> RIGHT
        KeyEvent.KEYCODE_DPAD_UP -> UP
        KeyEvent.KEYCODE_DPAD_DOWN -> DOWN
        KeyEvent.KEYCODE_MOVE_HOME -> HOME
        KeyEvent.KEYCODE_MOVE_END -> END
        KeyEvent.KEYCODE_PAGE_UP -> PAGE_UP
        KeyEvent.KEYCODE_PAGE_DOWN -> PAGE_DOWN
        in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F12 -> f(code - KeyEvent.KEYCODE_F1 + 1)
        else -> null
    }
}

// Vista invisible que recibe el teclado del teléfono y lo envía al PC.
// - Texto (tildes, ñ, emojis): mensaje Text; el host lo escribe tal cual.
// - Borrar, Enter, flechas...: teclas por scancode.
// - Con Ctrl/Alt/Mayús fijados en la barra de teclas: la letra se manda como tecla (Ctrl+C, Alt+F4...).
class KeyboardInput(context: Context, private val send: (ByteArray) -> Unit) : View(context) {
    // Modificadores de un solo uso (se sueltan tras la siguiente tecla).
    var ctrl = false
    var alt = false
    var shift = false
    var onModifiersChanged: (() -> Unit)? = null

    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onCheckIsTextEditor() = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        // Sin sugerencias ni autocorrección: cada letra se escribe en el PC apenas se toca.
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_ACTION_NONE
        return object : BaseInputConnection(this, false) {
            override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
                typeText(text.toString())
                return true
            }

            override fun setComposingText(text: CharSequence, newCursorPosition: Int): Boolean {
                // Teclados que componen (p. ej. con acentos muertos): se escribe al confirmar.
                composing = text.toString()
                return true
            }

            override fun finishComposingText(): Boolean {
                if (composing.isNotEmpty()) typeText(composing)
                composing = ""
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                repeat(beforeLength.coerceAtMost(64)) { tap(Scancodes.BACKSPACE) }
                repeat(afterLength.coerceAtMost(64)) { tap(Scancodes.DELETE) }
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                if (event.isSystem) return false
                if (event.action == KeyEvent.ACTION_DOWN) handleKey(event)
                return true
            }
        }
    }

    private var composing = ""

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        // Botones del teléfono (volumen, atrás, multimedia...): los maneja Android, no van al PC.
        if (event.isSystem || keyCode == KeyEvent.KEYCODE_BACK) return super.onKeyDown(keyCode, event)
        handleKey(event)
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean =
        if (event.isSystem || keyCode == KeyEvent.KEYCODE_BACK) super.onKeyUp(keyCode, event) else true

    private fun handleKey(event: KeyEvent) {
        Scancodes.forKeyCode(event.keyCode)?.let {
            tap(it)
            return
        }
        val ch = event.unicodeChar
        if (ch != 0) typeText(String(Character.toChars(ch)))
    }

    fun typeText(text: String) {
        if (text.isEmpty()) return
        if (ctrl || alt) {
            // Combinación: cada carácter como tecla física con los modificadores.
            for (c in text) Scancodes.forChar(c)?.let { tap(it) }
            return
        }
        // Enter dentro del texto como tecla (algunas aplicaciones no aceptan el carácter).
        val parts = text.split('\n')
        parts.forEachIndexed { i, part ->
            if (part.isNotEmpty()) send(Messages.text(part))
            if (i < parts.lastIndex) tap(Scancodes.ENTER)
        }
        clearModifiers()
    }

    fun tap(k: Scancodes.K) {
        val mods = buildList {
            if (ctrl) add(Scancodes.CTRL)
            if (alt) add(Scancodes.ALT)
            if (shift) add(Scancodes.SHIFT)
        }
        mods.forEach { send(Messages.key(it.code, it.extended, true)) }
        send(Messages.key(k.code, k.extended, true))
        send(Messages.key(k.code, k.extended, false))
        mods.asReversed().forEach { send(Messages.key(it.code, it.extended, false)) }
        clearModifiers()
    }

    private fun clearModifiers() {
        if (ctrl || alt || shift) {
            ctrl = false
            alt = false
            shift = false
            onModifiersChanged?.invoke()
        }
    }

    fun show() {
        requestFocus()
        (context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager).showSoftInput(this, 0)
    }

    fun hide() {
        (context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager).hideSoftInputFromWindow(windowToken, 0)
    }
}
