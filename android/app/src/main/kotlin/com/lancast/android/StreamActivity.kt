package com.lancast.android

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.graphics.RectF
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputFilter
import android.text.InputType
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import com.lancast.android.Ui.button
import com.lancast.android.Ui.dp
import com.lancast.android.Ui.row
import com.lancast.android.Ui.text
import com.lancast.core.AssembledFrame
import com.lancast.core.ClientCaps
import com.lancast.core.CodecChoice
import com.lancast.core.CursorPos
import com.lancast.core.CursorShape
import com.lancast.core.HostInfo
import com.lancast.core.Messages
import com.lancast.core.Session
import com.lancast.core.SessionListener
import com.lancast.core.StreamConfig
import com.lancast.core.StreamStarted

class StreamActivity : Activity(), SessionListener, TouchController.Host {
    private lateinit var storage: Storage
    private lateinit var address: String
    private var port = 47984

    private lateinit var root: FrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var cursorView: CursorView
    private lateinit var status: TextView
    private lateinit var stats: TextView
    private lateinit var toolbar: HorizontalScrollView
    private lateinit var tab: TextView
    private lateinit var keysBar: HorizontalScrollView
    private lateinit var bubble: TextView
    private lateinit var keyboard: KeyboardInput
    private lateinit var touch: TouchController

    private val ui = Handler(Looper.getMainLooper())
    private var session: Session? = null
    private var decoder: VideoDecoder? = null
    private val audio = AudioPlayer()
    private var wifiLock: WifiManager.WifiLock? = null
    @Volatile private var stream: StreamStarted? = null
    private var monitorId = 1
    private var connectedOnce = false
    private var reconnectUntil = 0L
    private var keyboardShown = false
    private var imeHeight = 0

    // Zoom de la vista (1 = todo el escritorio en pantalla).
    private var scale = 1f
    private var panX = 0f
    private var panY = 0f

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        storage = Storage(this)
        address = intent.getStringExtra(EXTRA_ADDRESS) ?: run {
            finish()
            return
        }
        port = intent.getIntExtra(EXTRA_PORT, 47984)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (Build.VERSION.SDK_INT >= 28) {
            // Usar también la zona de la cámara frontal (notch): más superficie para el escritorio.
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        buildUi()
        hideSystemBars()
    }

    override fun onStart() {
        super.onStart()
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        @Suppress("DEPRECATION")
        val mode = if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifiLock = wm.createWifiLock(mode, "lancast").apply {
            setReferenceCounted(false)
            acquire()
        }
        connect()
    }

    override fun onStop() {
        super.onStop()
        reconnectUntil = 0
        session?.close()
        session = null
        audio.close()
        wifiLock?.release()
        wifiLock = null
    }

    override fun onDestroy() {
        super.onDestroy()
        decoder?.close()
        decoder = null
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    // ------------------------------------------------------------------------------------------- conexión

    private fun connect() {
        session?.close()
        val (display, hz) = DeviceInfo.display(this)
        val decoders = DeviceInfo.hardwareDecoders()
        if (decoders.isEmpty()) {
            fatal("Este teléfono no tiene decodificador de video por hardware")
            return
        }
        val caps = ClientCaps(storage.deviceName, decoders, display.width, display.height, hz)
        val s = Session(storage.deviceId, storage.deviceName, storage, this)
        session = s
        setStatus(if (connectedOnce) "Reconectando…" else "Conectando a $address…")
        s.connect(address, port, caps, streamConfig())
    }

    private fun streamConfig(): StreamConfig {
        val (display, hz) = DeviceInfo.display(this)
        val (w, h) = when (storage.resolution) {
            "720" -> 1280 to 720
            "1080" -> 1920 to 1080
            "1440" -> 2560 to 1440
            else -> display.width to display.height
        }
        val codec = when (storage.codec) {
            "h264" -> CodecChoice.H264
            "hevc" -> CodecChoice.HEVC
            "av1" -> CodecChoice.AV1
            else -> CodecChoice.AUTO
        }
        return StreamConfig(
            monitorId = monitorId, maxWidth = w, maxHeight = h, fps = if (storage.fps == 0) hz else storage.fps,
            codec = codec, audio = storage.audio,
            autoBitrate = storage.bitrateMbps == 0, bitrateKbps = maxOf(storage.bitrateMbps, 5) * 1000,
        )
    }

    override fun onNeedPin(hostName: String) = ui.post {
        setStatus("Emparejando con $hostName")
        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_NUMBER
            filters = arrayOf(InputFilter.LengthFilter(6))
            textSize = 26f
            gravity = Gravity.CENTER
            hint = "000000"
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle("PIN de $hostName")
            .setMessage("Escribe el PIN de 6 dígitos que muestra el PC.")
            .setView(input)
            .setPositiveButton("Emparejar") { _, _ -> session?.submitPin(input.text.toString()) }
            .setNegativeButton("Cancelar") { _, _ -> finish() }
            .setCancelable(false)
            .create()
        dialog.setOnShowListener {
            input.requestFocus()
            dialog.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE)
        }
        dialog.show()
    }.let {}

    override fun onConnected(hostName: String) = ui.post {
        connectedOnce = true
        reconnectUntil = 0
        storage.addRecent(address)
        setStatus("Conectado a $hostName · esperando video…")
    }.let {}

    override fun onHostInfo(info: HostInfo) = ui.post {
        if (!info.inputAllowed) Toast.makeText(this, "${info.hostName} no permite el control: solo ver", Toast.LENGTH_LONG).show()
    }.let {}

    override fun onStreamStarted(stream: StreamStarted) {
        this.stream = stream
        ui.post {
            monitorId = stream.monitorId.takeIf { it != 0 } ?: monitorId
            surfaceView.holder.setFixedSize(stream.width, stream.height)
            layoutVideo()
            if (stream.notes.isNotBlank()) Toast.makeText(this, stream.notes.trim(), Toast.LENGTH_LONG).show()
        }
    }

    override fun onVideoFrame(frame: AssembledFrame, arrivalUs: Long) {
        val s = stream ?: return
        val d = decoder ?: return
        d.submit(frame, s)
        if (status.visibility == View.VISIBLE) ui.post { setStatus(null) }
    }

    override fun onAudio(pcm: ShortArray, sampleRate: Int, channels: Int) = audio.play(pcm, sampleRate, channels)

    override fun onCursorPos(pos: CursorPos) {
        ui.post {
            cursorView.shown = pos.visible
            touch.syncCursor(pos.x, pos.y)
            if (!touch.touching) placeCursor(touch.cx, touch.cy)
        }
    }

    override fun onCursorShape(shape: CursorShape) = ui.post { cursorView.setShape(shape) }.let {}

    override fun onMessage(text: String) = ui.post { Toast.makeText(this, text, Toast.LENGTH_LONG).show() }.let {}

    override fun onClosed(reason: String, retryable: Boolean) = ui.post {
        if (isFinishing || session == null) return@post
        // Corte de red: reintentar cada 2 s durante 1 minuto (ya emparejados, sin PIN).
        if (retryable && connectedOnce) {
            if (reconnectUntil == 0L) reconnectUntil = System.currentTimeMillis() + 60_000
            if (System.currentTimeMillis() < reconnectUntil) {
                setStatus("Se perdió la conexión · reconectando…")
                ui.postDelayed({ if (session != null && !isFinishing) connect() }, 2000)
                return@post
            }
        }
        fatal(reason)
    }.let {}

    private fun fatal(reason: String) {
        Toast.makeText(this, reason, Toast.LENGTH_LONG).show()
        session = null
        finish()
    }

    // ------------------------------------------------------------------------------------------- interfaz

    private fun buildUi() {
        root = FrameLayout(this).apply { setBackgroundColor(0xFF000000.toInt()) }
        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                decoder?.close()
                decoder = VideoDecoder(holder.surface, { session?.requestKeyframe() }, { msg -> ui.post { onMessage(msg) } })
                session?.requestKeyframe()
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                decoder?.close()
                decoder = null
            }
        })
        root.addView(surfaceView, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        cursorView = CursorView(this)
        root.addView(cursorView, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        touch = TouchController(this, resources.displayMetrics.density)
        touch.touchpad = storage.touchpad
        @Suppress("ClickableViewAccessibility")  // Es un touchpad: los gestos se traducen a clics del PC.
        cursorView.setOnTouchListener { _, e -> touch.onTouch(e) }

        keyboard = KeyboardInput(this) { send(it) }
        root.addView(keyboard, FrameLayout.LayoutParams(1, 1))

        status = text("", 16f).apply {
            setPadding(dp(18), dp(12), dp(18), dp(12))
            background = Ui.rounded(0xCC1C1F26.toInt(), dp(12).toFloat())
        }
        root.addView(status, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))

        stats = text("", 12f).apply {
            setPadding(dp(10), dp(8), dp(10), dp(8))
            background = Ui.rounded(0xB0000000.toInt(), dp(8).toFloat())
            visibility = if (storage.showStats) View.VISIBLE else View.GONE
            typeface = android.graphics.Typeface.MONOSPACE
        }
        root.addView(stats, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START).apply {
            setMargins(dp(12), dp(12), 0, 0)
        })

        toolbar = buildToolbar()
        root.addView(toolbar, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply {
            topMargin = dp(6)
        })
        toolbar.visibility = View.GONE

        // Pestaña pequeña arriba al centro: abre y cierra la barra sin tapar el escritorio.
        tab = text("▾", 18f).apply {
            gravity = Gravity.CENTER
            background = Ui.rounded(0xCC1C1F26.toInt(), dp(12).toFloat())
            setOnClickListener { toggleToolbar() }
        }
        root.addView(tab, FrameLayout.LayoutParams(dp(56), dp(26), Gravity.TOP or Gravity.CENTER_HORIZONTAL))

        keysBar = buildKeysBar()
        root.addView(keysBar, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM))
        keysBar.visibility = View.GONE

        bubble = buildBubble()
        root.addView(bubble, FrameLayout.LayoutParams(dp(52), dp(52), Gravity.TOP or Gravity.END).apply { marginEnd = dp(12) })
        bubble.visibility = if (storage.keyboardBubble) View.VISIBLE else View.GONE

        root.setOnApplyWindowInsetsListener { _, insets ->
            val wasOpen = imeHeight > 0
            imeHeight = if (Build.VERSION.SDK_INT >= 30) insets.getInsets(WindowInsets.Type.ime()).bottom
            else @Suppress("DEPRECATION") insets.systemWindowInsetBottom
            keysBar.translationY = -imeHeight.toFloat()
            // El teclado se cerró (p. ej. con el botón atrás): ocultar también la barra de teclas.
            if (wasOpen && imeHeight == 0 && keyboardShown) {
                keyboardShown = false
                keysBar.visibility = View.GONE
            }
            // Con el teclado abierto la vista sube a la zona libre; al cerrarlo, sin zoom, vuelve al centro.
            if (imeHeight == 0 && wasOpen && !zoomed()) {
                panX = 0f
                panY = 0f
            }
            clampPan()
            layoutVideo()
            placeBubble()
            insets
        }
        root.addOnLayoutChangeListener { _, l, t, r, b, ol, ot, or_, ob ->
            if (r - l != or_ - ol || b - t != ob - ot) clampPan()  // Giró la pantalla: la vista con zoom se reacomoda.
            layoutVideo()
            placeBubble()
        }
        setContentView(root)
        ui.post(statsTicker)
    }

    private fun buildToolbar(): HorizontalScrollView {
        val bar = row().apply {
            setPadding(dp(8), dp(6), dp(8), dp(6))
            background = Ui.rounded(0xE61C1F26.toInt(), dp(14).toFloat())
        }
        fun add(v: View) = bar.addView(v, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            marginEnd = dp(6)
        })
        add(button("⌨ Teclado") { toggleKeyboard() })
        lateinit var mode: TextView
        mode = button(if (touch.touchpad) "Modo: touchpad" else "Modo: directo") {
            touch.touchpad = !touch.touchpad
            storage.touchpad = touch.touchpad
            mode.text = if (touch.touchpad) "Modo: touchpad" else "Modo: directo"
            Toast.makeText(this, if (touch.touchpad) "El dedo mueve el cursor (como un touchpad)" else "Toca donde quieres hacer clic", Toast.LENGTH_SHORT).show()
        }
        add(mode)
        add(button("Monitor") { chooseMonitor() })
        add(button("Calidad") { chooseQuality() })
        add(button("Ctrl+Alt+Supr") {
            if (session?.hostInfo?.secureDesktop == true) send(Messages.secureAttention())
            else Toast.makeText(this, "Requiere LanCast instalado como servicio en el PC", Toast.LENGTH_LONG).show()
        })
        add(button("Zoom 1:1") { resetZoom() })
        lateinit var bubbleToggle: TextView
        bubbleToggle = button(if (storage.keyboardBubble) "Teclado flotante: sí" else "Teclado flotante: no") {
            storage.keyboardBubble = !storage.keyboardBubble
            bubble.visibility = if (storage.keyboardBubble) View.VISIBLE else View.GONE
            bubbleToggle.text = if (storage.keyboardBubble) "Teclado flotante: sí" else "Teclado flotante: no"
        }
        add(bubbleToggle)
        add(button("Estadísticas") {
            storage.showStats = !storage.showStats
            stats.visibility = if (storage.showStats) View.VISIBLE else View.GONE
        })
        add(button("Desconectar", danger = true) {
            reconnectUntil = 0
            session?.close()
            session = null
            finish()
        })
        return HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = false
            addView(bar)
        }
    }

    private fun buildKeysBar(): HorizontalScrollView {
        val bar = row().apply {
            setPadding(dp(6), dp(4), dp(6), dp(4))
            setBackgroundColor(0xF01C1F26.toInt())
        }
        fun key(label: String, action: () -> Unit) = button(label) { action() }.apply {
            textSize = 14f
            setPadding(dp(12), dp(6), dp(12), dp(6))
        }.also {
            bar.addView(it, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { marginEnd = dp(4) })
        }
        lateinit var ctrl: TextView
        lateinit var alt: TextView
        lateinit var shift: TextView
        fun refreshMods() {
            ctrl.background = Ui.rounded(if (keyboard.ctrl) Ui.ACCENT else 0xFF2A2E37.toInt(), dp(10).toFloat())
            alt.background = Ui.rounded(if (keyboard.alt) Ui.ACCENT else 0xFF2A2E37.toInt(), dp(10).toFloat())
            shift.background = Ui.rounded(if (keyboard.shift) Ui.ACCENT else 0xFF2A2E37.toInt(), dp(10).toFloat())
        }
        keyboard.onModifiersChanged = { refreshMods() }
        key("Esc") { keyboard.tap(Scancodes.ESC) }
        key("Tab") { keyboard.tap(Scancodes.TAB) }
        ctrl = key("Ctrl") {
            keyboard.ctrl = !keyboard.ctrl
            refreshMods()
        }
        alt = key("Alt") {
            keyboard.alt = !keyboard.alt
            refreshMods()
        }
        shift = key("Mayús") {
            keyboard.shift = !keyboard.shift
            refreshMods()
        }
        key("⊞ Win") { keyboard.tap(Scancodes.WIN) }
        key("←") { keyboard.tap(Scancodes.LEFT) }
        key("↑") { keyboard.tap(Scancodes.UP) }
        key("↓") { keyboard.tap(Scancodes.DOWN) }
        key("→") { keyboard.tap(Scancodes.RIGHT) }
        key("Inicio") { keyboard.tap(Scancodes.HOME) }
        key("Fin") { keyboard.tap(Scancodes.END) }
        key("Supr") { keyboard.tap(Scancodes.DELETE) }
        for (n in 1..12) key("F$n") { keyboard.tap(Scancodes.f(n)) }
        key("Ocultar") { toggleKeyboard() }
        return HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = false
            addView(bar)
        }
    }

    // Bolita flotante a la derecha: tocarla abre/cierra el teclado; arrastrarla la sube o la baja.
    private fun buildBubble(): TextView {
        val b = text("⌨", 22f).apply {
            gravity = Gravity.CENTER
            background = Ui.rounded(0xB34A90E2.toInt(), dp(26).toFloat(), dp(1), 0x80FFFFFF.toInt())
            elevation = dp(4).toFloat()
        }
        val slop = dp(8)
        var downRawY = 0f
        var startY = 0f
        var dragged = false
        @Suppress("ClickableViewAccessibility")
        b.setOnTouchListener { v, e ->
            when (e.actionMasked) {
                android.view.MotionEvent.ACTION_DOWN -> {
                    downRawY = e.rawY
                    startY = v.y
                    dragged = false
                }
                android.view.MotionEvent.ACTION_MOVE -> {
                    val dy = e.rawY - downRawY
                    if (!dragged && kotlin.math.abs(dy) > slop) dragged = true
                    if (dragged) {
                        val (top, bottom) = bubbleRange()
                        v.y = (startY + dy).coerceIn(top, bottom)
                    }
                }
                android.view.MotionEvent.ACTION_UP -> {
                    if (dragged) {
                        val (top, bottom) = bubbleRange()
                        if (bottom > top) storage.bubbleY = (v.y - top) / (bottom - top)
                    } else {
                        toggleKeyboard()
                    }
                }
            }
            true
        }
        return b
    }

    // Zona vertical permitida para la bolita (sin taparse con el teclado del teléfono).
    private fun bubbleRange(): Pair<Float, Float> {
        val size = dp(52).toFloat()
        val top = dp(40).toFloat()
        val bottom = maxOf(top, root.height - imeHeight - size - dp(if (keyboardShown) 56 else 12))
        return top to bottom
    }

    private fun placeBubble() {
        if (!::bubble.isInitialized || root.height == 0) return
        val (top, bottom) = bubbleRange()
        bubble.y = top + storage.bubbleY.coerceIn(0f, 1f) * (bottom - top)
    }

    private fun toggleToolbar() {
        val show = toolbar.visibility != View.VISIBLE
        toolbar.visibility = if (show) View.VISIBLE else View.GONE
        tab.text = if (show) "✕" else "▾"
        (tab.layoutParams as FrameLayout.LayoutParams).topMargin = if (show) toolbar.height.coerceAtLeast(dp(52)) + dp(8) else 0
        tab.requestLayout()
    }

    override fun toggleKeyboard() {
        keyboardShown = !keyboardShown
        if (keyboardShown) {
            keysBar.visibility = View.VISIBLE
            keyboard.show()
        } else {
            keysBar.visibility = View.GONE
            keyboard.hide()
            hideSystemBars()
        }
        placeBubble()
    }

    private fun chooseMonitor() {
        val info = session?.hostInfo ?: return
        val items = info.monitors.map { "${it.id}: ${it.name} (${it.rect.width}x${it.rect.height})" } + "Todos los monitores"
        AlertDialog.Builder(this).setTitle("Monitor del PC").setItems(items.toTypedArray()) { _, i ->
            monitorId = if (i < info.monitors.size) info.monitors[i].id else 0
            session?.requestStream(streamConfig())
        }.show()
    }

    private fun chooseQuality() {
        val resolutions = listOf("screen" to "Igual a esta pantalla", "720" to "720p", "1080" to "1080p", "1440" to "1440p")
        val bitrates = listOf(20, 40, 60, 100, 0)
        val items = resolutions.map { "Resolución: ${it.second}" } + bitrates.map { if (it == 0) "Bitrate: automático" else "Bitrate: $it Mbps" }
        AlertDialog.Builder(this).setTitle("Calidad").setItems(items.toTypedArray()) { _, i ->
            if (i < resolutions.size) storage.resolution = resolutions[i].first else storage.bitrateMbps = bitrates[i - resolutions.size]
            session?.requestStream(streamConfig())
            Toast.makeText(this, "Aplicando: ${items[i]}", Toast.LENGTH_SHORT).show()
        }.show()
    }

    private fun setStatus(text: String?) {
        if (text == null) {
            status.visibility = View.GONE
        } else {
            status.text = text
            status.visibility = View.VISIBLE
        }
    }

    private val statsTicker = object : Runnable {
        override fun run() {
            if (storage.showStats) {
                val st = session?.stats
                val s = stream
                stats.text = buildString {
                    if (s != null) append("${s.codec.label} ${s.width}x${s.height} @ ${s.fps} fps · ${s.encoderBackend}\n")
                    if (st != null) {
                        append("FPS %.0f · %.1f Mbps\n".format(st.fps, st.bitrateMbps))
                        append("Red %.1f ms · RTT %.1f ms\n".format(st.networkMs, st.rttMs))
                        append("Decodificación %.1f ms\n".format(decoder?.decodeMs ?: 0.0))
                        append("FEC %d · perdidos %d · IDR %d\n".format(st.framesRecovered, st.framesLost, st.keyframeRequests))
                    }
                    append(decoder?.name ?: "")
                }
            }
            ui.postDelayed(this, 500)
        }
    }

    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        }
    }

    // ------------------------------------------------------------------------------------------- vista y zoom

    private fun baseRect(): RectF {
        val s = stream
        val w = root.width.toFloat()
        val h = root.height.toFloat()
        if (s == null || w <= 0 || h <= 0) return RectF(0f, 0f, w, h)
        val k = minOf(w / s.width, h / s.height)
        val vw = s.width * k
        val vh = s.height * k
        return RectF((w - vw) / 2, (h - vh) / 2, (w + vw) / 2, (h + vh) / 2)
    }

    override fun videoRect(): RectF {
        val b = baseRect()
        val w = b.width() * scale
        val h = b.height() * scale
        val left = b.centerX() - w / 2 + panX
        val top = b.centerY() - h / 2 + panY
        return RectF(left, top, left + w, top + h)
    }

    override fun zoomed() = scale > 1.01f

    // Alto de la pantalla que no tapa el teclado (ni su barra de teclas).
    private fun visibleHeight(): Float {
        val keys = if (keyboardShown && keysBar.visibility == View.VISIBLE) keysBar.height else 0
        return (root.height - imeHeight - keys).toFloat().coerceAtLeast(root.height / 4f)
    }

    // La vista puede moverse a cualquier lugar donde se vea entera; si es más grande que la pantalla (zoom), sus
    // bordes no pueden despegarse de los de la pantalla. En vertical esto permite subirla sobre el teclado.
    private fun clampPan() {
        val b = baseRect()
        val w = b.width() * scale
        val h = b.height() * scale
        fun clampAxis(pan: Float, center: Float, size: Float, visible: Float): Float {
            val left = center - size / 2 + pan
            val lo = if (size <= visible) 0f else visible - size
            val hi = if (size <= visible) visible - size else 0f
            return pan + (left.coerceIn(lo, hi) - left)
        }
        panX = clampAxis(panX, b.centerX(), w, root.width.toFloat())
        panY = clampAxis(panY, b.centerY(), h, visibleHeight())
    }

    override fun zoomBy(factor: Float, focusX: Float, focusY: Float) {
        val before = videoRect()
        val newScale = (scale * factor).coerceIn(1f, 5f)
        if (newScale == scale) return
        // Mantener el punto bajo los dedos en su lugar.
        val fx = (focusX - before.left) / before.width()
        val fy = (focusY - before.top) / before.height()
        scale = newScale
        val after = videoRect()
        panX += focusX - (after.left + fx * after.width())
        panY += focusY - (after.top + fy * after.height())
        clampPan()
        layoutVideo()
    }

    override fun panBy(dx: Float, dy: Float) {
        panX += dx
        panY += dy
        clampPan()
        layoutVideo()
    }

    private fun resetZoom() {
        scale = 1f
        panX = 0f
        panY = 0f
        layoutVideo()
    }

    private fun layoutVideo() {
        val r = videoRect()
        val lp = surfaceView.layoutParams as FrameLayout.LayoutParams
        val w = r.width().toInt().coerceAtLeast(1)
        val h = r.height().toInt().coerceAtLeast(1)
        if (lp.width != w || lp.height != h || lp.gravity != (Gravity.TOP or Gravity.START)) {
            lp.width = w
            lp.height = h
            lp.gravity = Gravity.TOP or Gravity.START
            surfaceView.layoutParams = lp
        }
        surfaceView.x = r.left
        surfaceView.y = r.top
        placeCursor(touch.cx, touch.cy)
    }

    private fun placeCursor(nx: Float, ny: Float) {
        val r = videoRect()
        val s = stream
        cursorView.scale = if (s != null) r.width() / s.source.width.coerceAtLeast(1) else 1f
        cursorView.moveTo(r.left + nx * r.width(), r.top + ny * r.height())
    }

    // ------------------------------------------------------------------------------------------- TouchController.Host

    override fun send(message: ByteArray) {
        session?.send(message)
    }

    override fun cursorMoved(nx: Float, ny: Float) {
        cursorView.shown = true
        if (zoomed()) {
            // Con zoom, la vista sigue al cursor para que nunca quede fuera de la pantalla.
            val r = videoRect()
            val px = r.left + nx * r.width()
            val py = r.top + ny * r.height()
            val m = dp(48).toFloat()
            var dx = 0f
            var dy = 0f
            if (px < m) dx = m - px else if (px > root.width - m) dx = root.width - m - px
            if (py < m) dy = m - py else if (py > visibleHeight() - m) dy = visibleHeight() - m - py
            if (dx != 0f || dy != 0f) {
                panBy(dx, dy)
                return
            }
        }
        placeCursor(nx, ny)
    }

    override fun haptic() {
        cursorView.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
    }

    companion object {
        const val EXTRA_ADDRESS = "address"
        const val EXTRA_PORT = "port"
    }
}
