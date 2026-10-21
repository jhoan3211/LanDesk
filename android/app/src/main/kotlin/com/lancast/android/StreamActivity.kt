package com.lancast.android

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.pm.PackageManager
import android.content.Intent
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
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
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
import com.lancast.core.FileProgress
import com.lancast.core.FileTransfer
import com.lancast.core.GamepadRumble
import com.lancast.core.MouseButtonId
import com.lancast.core.HostInfo
import com.lancast.core.HostServer
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

    private lateinit var graphs: GraphView
    private lateinit var filesStatus: TextView
    private var pad: OnScreenPad? = null
    private lateinit var gamepads: GamepadInput
    private val mic = MicSender { session }
    private var micButton: TextView? = null
    private var files: FileTransfer? = null
    private var pickingFiles = false  // El selector de archivos tapa la pantalla: la sesión sigue viva.
    @Volatile private var lastRemoteClipboard: String? = null
    private var lastSentClipboard: String? = null
    private var mouseButtons = 0
    private var stylusButton: MouseButtonId? = null
    private val decodeHistory = ArrayDeque<Float>()

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
        volumeControlStream = android.media.AudioManager.STREAM_MUSIC  // Los botones de volumen controlan el audio del PC.
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
        WifiBinding.start(this)
        if (pickingFiles && session != null) {
            pickingFiles = false
            return
        }
        pickingFiles = false
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
        if (pickingFiles) return
        session?.history()?.let { if (it.isNotEmpty()) AppLog.lastStats = it }
        gamepads.disconnectAll()
        mic.stop()
        micButton?.text = "Micrófono: no"
        pad?.release()
        files?.close()
        files = null
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
        if (hasFocus) {
            hideSystemBars()
            ui.postDelayed({ sendClipboard() }, 300)  // Android solo deja leer el portapapeles con la app enfocada.
        }
    }

    // ------------------------------------------------------------------------------------------- portapapeles

    private fun clipboard() = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager

    // Teléfono -> PC: lo copiado en otra app llega al PC al volver a LanCast.
    private fun sendClipboard() {
        if (session?.hostInfo?.clipboardAllowed != true) return
        val text = try {
            clipboard().primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(this)?.toString()
        } catch (_: Exception) {
            null
        } ?: return
        if (text.isEmpty() || text.length > 1_000_000 || text == lastRemoteClipboard || text == lastSentClipboard) return
        lastSentClipboard = text
        send(Messages.clipboard(text))
    }

    // PC -> teléfono.
    override fun onRumble(rumble: GamepadRumble) = ui.post {
        if (::gamepads.isInitialized && session != null) gamepads.rumble(this, rumble)
    }.let {}

    override fun onClipboard(text: String) {
        if (session?.hostInfo?.clipboardAllowed != true) return
        lastRemoteClipboard = text
        ui.post { runCatching { clipboard().setPrimaryClip(ClipData.newPlainText("LanCast", text)) } }
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
        val s = Session(storage.deviceId, storage.deviceName, storage, this) { DeviceInfo.broadcastAddresses() }
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
            pacingMbps = storage.pacingMbps,
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
        // Si el equipo cambió de IP (DHCP), se guarda la nueva.
        val real = session?.hostAddress?.hostAddress
        if (real != null && real != address && storage.recent.contains(address)) storage.recent = storage.recent.map { if (it == address) real else it }
        storage.addRecent(real ?: address)
        setStatus("Conectado a $hostName · esperando video…")
    }.let {}

    override fun onHostInfo(info: HostInfo) = ui.post {
        if ((!info.inputAllowed || !info.micAvailable) && mic.active) {  // Pasó a "solo ver": el micrófono se apaga.
            mic.stop()
            micButton?.text = "Micrófono: no"
        }
        if (info.spectator) {
            val who = if (info.controllerName.isNotEmpty()) " (controla ${info.controllerName})" else ""
            Toast.makeText(this, "Espectador$who: solo ves y escuchas", Toast.LENGTH_LONG).show()
        } else if (!info.inputAllowed) {
            val why = if (info.platform == HostServer.PLATFORM_ANDROID) "no activó el control remoto (Accesibilidad)" else "no permite el control"
            Toast.makeText(this, "${info.hostName} $why: solo ver", Toast.LENGTH_LONG).show()
        }
        if (info.platform == HostServer.PLATFORM_ANDROID && touch.touchpad) {
            touch.touchpad = false
            Toast.makeText(this, "Es un teléfono: toca donde quieras tocar en él", Toast.LENGTH_SHORT).show()
        }
        if (info.hasMac) {
            storage.saveMac(address, info.mac)
            session?.hostAddress?.hostAddress?.let { storage.saveMac(it, info.mac) }
        }
        sendClipboard()
        openFiles()
    }.let {}

    // ------------------------------------------------------------------------------------------- archivos

    private fun openFiles() {
        if (files != null) return
        val s = session ?: return
        if (s.hostInfo?.filesAllowed != true) return
        Thread {
            val ft = s.openFiles(FileSaver(this)) { p -> ui.post { showFileProgress(p) } }
            ui.post { if (session === s) files = ft else ft?.close() }
        }.apply { isDaemon = true }.start()
    }

    private fun showFileProgress(p: FileProgress) {
        val what = if (p.outgoing) "Enviando" else "Recibiendo"
        if (p.finished) {
            filesStatus.visibility = View.GONE
            val msg = when {
                p.ok && p.outgoing -> "${p.name} enviado al PC"
                p.ok -> "${p.name} guardado en ${FileSaver.folderLabel()}"
                else -> "${p.name}: ${p.message}"
            }
            Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
            return
        }
        val pct = if (p.size > 0) (p.done * 100 / p.size).toInt() else 0
        filesStatus.text = if (p.message.isNotEmpty() && p.done == 0L) "$what ${p.name}: ${p.message}" else "$what ${p.name} · $pct %"
        filesStatus.visibility = View.VISIBLE
    }

    private fun pickFiles() {
        if (session?.hostInfo?.filesAllowed != true) {
            Toast.makeText(this, "El PC no permite recibir archivos (Ajustes del host)", Toast.LENGTH_LONG).show()
            return
        }
        if (files == null) {
            openFiles()
            Toast.makeText(this, "Abriendo el canal de archivos… inténtalo de nuevo en un momento", Toast.LENGTH_SHORT).show()
            return
        }
        pickingFiles = true
        @Suppress("DEPRECATION")
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        }, REQUEST_FILES)
    }

    @Deprecated("API de actividad clásica: suficiente aquí")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_FILES || resultCode != RESULT_OK || data == null) return
        val ft = files ?: return
        val uris = buildList {
            data.clipData?.let { c -> for (i in 0 until c.itemCount) add(c.getItemAt(i).uri) }
            if (isEmpty()) data.data?.let { add(it) }
        }
        for (uri in uris) {
            var name = "archivo"
            var size = -1L
            contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME, android.provider.OpenableColumns.SIZE),
                null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    name = c.getString(0) ?: name
                    if (!c.isNull(1)) size = c.getLong(1)
                }
            }
            if (size < 0) {
                Toast.makeText(this, "$name: no se pudo saber su tamaño", Toast.LENGTH_LONG).show()
                continue
            }
            ft.send(name, size, 0) { contentResolver.openInputStream(uri) ?: throw java.io.IOException("No se pudo abrir") }
        }
    }

    override fun onStreamStarted(stream: StreamStarted) {
        this.stream = stream
        AppLog.lastStream = "${stream.codec.label} ${stream.width}x${stream.height} @ ${stream.fps} fps · ${stream.bitrateKbps} kbps · ${stream.encoderBackend}"
        AppLog.add("Stream con $address: ${AppLog.lastStream}")
        ui.post {
            monitorId = stream.monitorId.takeIf { it != 0 } ?: monitorId
            surfaceView.holder.setFixedSize(stream.width, stream.height)
            matchRefreshRate(stream.fps)
            layoutVideo()
            if (stream.notes.isNotBlank()) Toast.makeText(this, stream.notes.trim(), Toast.LENGTH_LONG).show()
        }
    }

    // La pantalla a la frecuencia del stream (p. ej. 120 Hz): sin esto, Android puede bajarla a 60 Hz porque cree que
    // es un video, y los cuadros se ven a saltos.
    private fun matchRefreshRate(fps: Int) {
        if (fps <= 0) return
        if (Build.VERSION.SDK_INT >= 30) {
            runCatching { surfaceView.holder.surface.setFrameRate(fps.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE) }
        }
        @Suppress("DEPRECATION")
        val disp = if (Build.VERSION.SDK_INT >= 30) display else windowManager.defaultDisplay
        val current = disp?.mode ?: return
        // Mismo tamaño; la frecuencia más baja que sea múltiplo de los FPS (o, si no hay, la más alta).
        val modes = disp.supportedModes.filter { it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight }
        val best = modes.filter { m -> m.refreshRate >= fps - 1 && Math.round(m.refreshRate) % fps <= 1 }.minByOrNull { it.refreshRate }
            ?: modes.maxByOrNull { it.refreshRate } ?: return
        if (window.attributes.preferredDisplayModeId != best.modeId) {
            window.attributes = window.attributes.apply { preferredDisplayModeId = best.modeId }
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

    override fun onMessage(text: String) = ui.post {
        AppLog.add("Aviso: $text")
        Toast.makeText(this, text, Toast.LENGTH_LONG).show()
    }.let {}

    override fun onClosed(reason: String, retryable: Boolean) = ui.post {
        AppLog.add("Conexión con $address cerrada: $reason")
        session?.history()?.let { if (it.isNotEmpty()) AppLog.lastStats = it }
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
        cursorView.setOnTouchListener { _, e -> pointerEvent(e) || touch.onTouch(e) }
        gamepads = GamepadInput { send(it) }

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

        graphs = GraphView(this).apply { visibility = if (storage.showGraphs) View.VISIBLE else View.GONE }
        root.addView(graphs, FrameLayout.LayoutParams(dp(300), dp(260), Gravity.TOP or Gravity.END).apply {
            setMargins(0, dp(12), dp(76), 0)
        })

        filesStatus = text("", 13f).apply {
            setPadding(dp(12), dp(8), dp(12), dp(8))
            background = Ui.rounded(0xCC1C1F26.toInt(), dp(10).toFloat())
            visibility = View.GONE
        }
        root.addView(filesStatus, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM or Gravity.START).apply { setMargins(dp(12), 0, 0, dp(12)) })

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
        add(button("Archivos") { pickFiles() })
        add(button("Mando") { togglePad() })
        micButton = button(if (mic.active) "Micrófono: sí" else "Micrófono: no") { toggleMic() }
        add(micButton!!)
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
        add(button("Gráficos") {
            storage.showGraphs = !storage.showGraphs
            graphs.visibility = if (storage.showGraphs) View.VISIBLE else View.GONE
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
        val pacing = listOf(0, 300, 500)
        val items = resolutions.map { "Resolución: ${it.second}" } + bitrates.map { if (it == 0) "Bitrate: automático" else "Bitrate: $it Mbps" } +
            pacing.map { if (it == 0) "Ritmo de envío: sin límite" else "Ritmo de envío: $it Mbps (si hay cortes por Wi-Fi)" }
        AlertDialog.Builder(this).setTitle("Calidad").setItems(items.toTypedArray()) { _, i ->
            when {
                i < resolutions.size -> storage.resolution = resolutions[i].first
                i < resolutions.size + bitrates.size -> storage.bitrateMbps = bitrates[i - resolutions.size]
                else -> storage.pacingMbps = pacing[i - resolutions.size - bitrates.size]
            }
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
            decodeHistory.addLast((decoder?.decodeMs ?: 0.0).toFloat())
            while (decodeHistory.size > GraphView.SLOTS) decodeHistory.removeFirst()
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
            if (storage.showGraphs) graphs.update(session?.history() ?: emptyList(), decodeHistory.toList())
            ui.postDelayed(this, 250)
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
        private const val REQUEST_FILES = 7
        private const val REQUEST_MIC = 8
    }

    // Micrófono del teléfono -> PC (llega allá como un micrófono si el PC tiene un cable de audio virtual).
    private fun toggleMic() {
        if (mic.active) {
            mic.stop()
        } else if (session?.hostInfo?.inputAllowed != true) {
            Toast.makeText(this, "En modo solo ver no se puede hablar por el micrófono", Toast.LENGTH_LONG).show()
        } else if (session?.hostInfo?.micAvailable != true) {
            AlertDialog.Builder(this).setTitle("Micrófono")
                .setMessage("Para que tu micrófono llegue al PC como un micrófono más (Discord, Teams, juegos), el PC necesita " +
                    "un cable de audio virtual: instala VB-Cable (gratis, vb-audio.com/Cable), deja activado \"Recibir el " +
                    "micrófono del cliente\" en LanCast y vuelve a conectar. Allá elige el micrófono \"CABLE Output\".")
                .setPositiveButton("Entendido", null).show()
        } else if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQUEST_MIC)
        } else if (!mic.start()) {
            Toast.makeText(this, "No se pudo abrir el micrófono", Toast.LENGTH_LONG).show()
        }
        micButton?.text = if (mic.active) "Micrófono: sí" else "Micrófono: no"
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_MIC && grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED) toggleMic()
    }

    // ------------------------------------------------------------------------------------------- mando, mouse, S Pen y teclado físico

    private fun togglePad() {
        val p = pad
        if (p != null) {
            p.release()
            root.removeView(p)
            pad = null
            return
        }
        if (session?.hostInfo?.gamepadAvailable != true) {
            AlertDialog.Builder(this).setTitle("Mando")
                .setMessage("Para usar mandos, el PC necesita el driver ViGEmBus 1.22.0 " +
                    "(github.com/nefarius/ViGEmBus/releases/tag/v1.22.0) y permitir el control. Es gratuito y funciona en " +
                    "Windows 10 y 11, pero su autor lo dejó de mantener en 2023: es la última versión y no tendrá más " +
                    "actualizaciones. Instálalo en el PC y vuelve a conectar.")
                .setPositiveButton("Entendido", null).show()
            return
        }
        val v = OnScreenPad(this) { send(it) }
        root.addView(v, root.indexOfChild(cursorView) + 1,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        pad = v
        Toast.makeText(this, "Mando en pantalla: aparece en el PC como un mando de Xbox", Toast.LENGTH_SHORT).show()
    }

    private fun pointTo(x: Float, y: Float) {
        val r = videoRect()
        if (r.width() <= 0 || r.height() <= 0) return
        val nx = ((x - r.left) / r.width()).coerceIn(0f, 1f)
        val ny = ((y - r.top) / r.height()).coerceIn(0f, 1f)
        touch.setCursor(nx, ny)
        send(Messages.mouseAbs(nx, ny))
        cursorView.shown = true
        placeCursor(nx, ny)
    }

    // Mouse (Bluetooth/USB) y S Pen: el puntero va exactamente donde apunta, como en el PC.
    private fun pointerEvent(e: MotionEvent): Boolean {
        val tool = e.getToolType(0)
        val mouse = tool == MotionEvent.TOOL_TYPE_MOUSE
        val stylus = tool == MotionEvent.TOOL_TYPE_STYLUS || tool == MotionEvent.TOOL_TYPE_ERASER
        if (!mouse && !stylus) return false
        if (e.actionMasked == MotionEvent.ACTION_SCROLL) {
            val v = e.getAxisValue(MotionEvent.AXIS_VSCROLL)
            val h = e.getAxisValue(MotionEvent.AXIS_HSCROLL)
            if (v != 0f) send(Messages.mouseWheel(Math.round(v * 120)))
            if (h != 0f) send(Messages.mouseWheel(Math.round(h * 120), true))
            return true
        }
        pointTo(e.x, e.y)
        if (mouse) {
            // Botones por diferencia de estado (según el teléfono llegan como toque o como evento genérico).
            val now = e.buttonState
            val changed = now xor mouseButtons
            fun button(mask: Int, id: MouseButtonId) {
                if (changed and mask != 0) send(Messages.mouseButton(id, now and mask != 0))
            }
            button(MotionEvent.BUTTON_PRIMARY, MouseButtonId.LEFT)
            button(MotionEvent.BUTTON_SECONDARY, MouseButtonId.RIGHT)
            button(MotionEvent.BUTTON_TERTIARY, MouseButtonId.MIDDLE)
            mouseButtons = now
        } else {
            // S Pen: apoyar la punta = clic izquierdo (y arrastrar); con el botón lateral apretado = clic derecho.
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    val b = if (e.buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY != 0) MouseButtonId.RIGHT else MouseButtonId.LEFT
                    stylusButton = b
                    send(Messages.mouseButton(b, true))
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> stylusButton?.let {
                    send(Messages.mouseButton(it, false))
                    stylusButton = null
                }
            }
        }
        return true
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (gamepads.onMotion(event)) return true
        // Mouse o lápiz moviéndose sin tocar (hover), la rueda y los botones del mouse.
        if (event.isFromSource(InputDevice.SOURCE_CLASS_POINTER) && ::cursorView.isInitialized) {
            val loc = IntArray(2)
            cursorView.getLocationOnScreen(loc)
            val copy = MotionEvent.obtain(event)
            copy.setLocation(event.rawX - loc[0], event.rawY - loc[1])
            val handled = pointerEvent(copy)
            copy.recycle()
            if (handled) return true
        }
        return super.dispatchGenericMotionEvent(event)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (::gamepads.isInitialized && gamepads.onKey(event)) return true
        // El clic derecho de un mouse llega como "atrás": no debe cerrar el stream.
        if (event.keyCode == KeyEvent.KEYCODE_BACK && event.isFromSource(InputDevice.SOURCE_MOUSE)) return true
        // Teclado físico: cada tecla con pulsar y soltar (atajos, juegos, Ctrl+Alt...).
        val dev = event.device
        if (dev != null && !dev.isVirtual && dev.keyboardType == InputDevice.KEYBOARD_TYPE_ALPHABETIC && !event.isSystem &&
            session?.hostInfo?.inputAllowed == true) {
            val k = Scancodes.forHardware(event.keyCode)
            if (k != null) {
                when (event.action) {
                    KeyEvent.ACTION_DOWN -> send(Messages.key(k.code, k.extended, true))
                    KeyEvent.ACTION_UP -> send(Messages.key(k.code, k.extended, false))
                }
                return true
            }
        }
        return super.dispatchKeyEvent(event)
    }
}
