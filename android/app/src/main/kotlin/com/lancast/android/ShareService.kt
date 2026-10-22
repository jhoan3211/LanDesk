package com.lancast.android

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.DisplayMetrics
import android.view.WindowManager
import com.lancast.core.ClientInfo
import com.lancast.core.Codec
import com.lancast.core.HostDescription
import com.lancast.core.HostListener
import com.lancast.core.HostServer
import com.lancast.core.RemoteInput
import com.lancast.core.StreamRequest

// Comparte la pantalla de este teléfono: captura (MediaProjection) -> codificador por hardware -> host de LanCast.
// El control llega al servicio de accesibilidad (ControlService), si el usuario lo activó.
class ShareService : Service(), HostListener {
    // Estado para la pantalla principal.
    data class State(val sharing: Boolean = false, val client: String? = null, val pin: String? = null, val pinFor: String = "",
                     val detail: String = "")

    private lateinit var storage: Storage
    private val ui = Handler(Looper.getMainLooper())
    private var projection: MediaProjection? = null
    private var display: VirtualDisplay? = null
    private var encoder: ScreenEncoder? = null
    private var server: HostServer? = null
    private var audio: AudioRecord? = null
    @Volatile private var audioRunning = false
    private var wifiLock: WifiManager.WifiLock? = null
    private var lastRequest: StreamRequest? = null
    private var lastClient: ClientInfo? = null
    private val encoderLock = Any()

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (projection != null) return START_NOT_STICKY
        storage = Storage(this)
        // Android 14+: el servicio debe estar en primer plano (tipo mediaProjection) antes de pedir la captura.
        startForegroundCompat(notification("Esperando que alguien se conecte", null))
        val code = intent?.getIntExtra(EXTRA_CODE, 0) ?: 0
        @Suppress("DEPRECATION")
        val data: Intent? = if (Build.VERSION.SDK_INT >= 33) intent?.getParcelableExtra(EXTRA_DATA, Intent::class.java)
        else intent?.getParcelableExtra(EXTRA_DATA)
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        val p = data?.let { runCatching { mpm.getMediaProjection(code, it) }.getOrNull() }
        if (p == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        projection = p
        p.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                ui.post { stopSelf() }  // El usuario detuvo la captura desde el sistema.
            }
        }, ui)
        // Una sola pantalla virtual por captura (Android 14+): después solo se cambia su tamaño y su Surface.
        val (w, h, dpi) = screen()
        display = p.createVirtualDisplay("LanCast", w, h, dpi, DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, null, null, ui)
        (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager).registerDisplayListener(displayListener, ui)

        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        @Suppress("DEPRECATION")
        val mode = if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifiLock = wm.createWifiLock(mode, "lancast-share").apply {
            setReferenceCounted(false)
            acquire()
        }
        WifiBinding.start(this)

        val s = HostServer(storage.deviceId, storage.hostClients(), this) { describe() }
        if (!s.start()) {
            update(State(false, detail = "No se pudo abrir el puerto de LanCast (¿otra app lo usa?)"))
            stopSelf()
            return START_NOT_STICKY
        }
        server = s
        update(State(true, detail = "Esperando conexión en ${DeviceInfo.localIp() ?: "esta red"}"))
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        super.onDestroy()
        runCatching { (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager).unregisterDisplayListener(displayListener) }
        server?.stop()
        server = null
        stopAudio()
        synchronized(encoderLock) {
            encoder?.release()
            encoder = null
        }
        display?.release()
        display = null
        projection?.stop()
        projection = null
        wifiLock?.release()
        ControlService.instance?.releaseAll()
        update(State(false))
    }

    // --------------------------------------------------------------------------------------- HostServer

    private fun describe(): HostDescription {
        val (w, h, _) = screen()
        return HostDescription(storage.deviceName, w, h, refreshHz(), ScreenEncoder.availableCodecs(), audioAvailable(),
            inputAllowed = ControlService.instance != null, clipboardAllowed = true)
    }

    override fun onPin(pin: String, requester: String) {
        update(current.copy(pin = pin, pinFor = requester))
        notify(notification("PIN para $requester: $pin", "Escríbelo en el otro equipo"))
    }

    override fun onPinDone() {
        update(current.copy(pin = null))
    }

    override fun onSessionStarted(client: ClientInfo, address: String) {
        lastClient = client
        update(current.copy(client = client.name.ifEmpty { address }, pin = null, detail = "Conectado: $address"))
        notify(notification("${client.name.ifEmpty { address }} está viendo esta pantalla", null))
        // Puntero para el cliente de PC (la pantalla del teléfono no tiene cursor).
        server?.sendCursorShape(ARROW_W, ARROW_H, 0, 0, arrow())
    }

    override fun onSessionEnded(clientName: String) {
        stopAudio()
        synchronized(encoderLock) {
            display?.surface = null
            encoder?.release()
            encoder = null
        }
        ControlService.instance?.releaseAll()
        lastRequest = null
        update(current.copy(client = null, detail = "Esperando conexión en ${DeviceInfo.localIp() ?: "esta red"}"))
        notify(notification("Esperando que alguien se conecte", null))
    }

    override fun onStreamRequested(client: ClientInfo, request: StreamRequest) {
        lastClient = client
        lastRequest = request
        ui.post { configure(client, request) }
    }

    override fun onKeyframeRequested() {
        encoder?.requestKeyframe()
    }

    override fun onInput(input: RemoteInput) {
        when (input) {
            is RemoteInput.MouseAbs -> server?.sendCursor(input.x, input.y, true)
            is RemoteInput.Clipboard -> ui.post {
                runCatching {
                    (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(ClipData.newPlainText("LanCast", input.text))
                }
            }
            else -> {}
        }
        ControlService.instance?.handle(input)
    }

    // --------------------------------------------------------------------------------------- captura

    private fun configure(client: ClientInfo, req: StreamRequest) {
        val s = server ?: return
        val d = display ?: return
        val available = ScreenEncoder.availableCodecs()
        if (available.isEmpty()) return
        // HEVC si los dos lo tienen (mejor imagen con el mismo bitrate), si no H.264.
        val wantHevc = req.codec == 0 || req.codec == 2
        val codec = if (wantHevc && Codec.HEVC in available && (client.decoders.isEmpty() || Codec.HEVC in client.decoders)) Codec.HEVC
        else Codec.H264
        val (sw, sh, dpi) = screen()
        val (w, h) = ScreenEncoder.fitSize(codec, sw, sh, req.maxWidth, req.maxHeight)
        val fps = req.fps.coerceIn(10, refreshHz().coerceAtMost(120))
        val kbps = if (req.autoBitrate) (w.toLong() * h * fps / 12_000).toInt().coerceIn(4_000, 60_000) else req.bitrateKbps.coerceIn(1_000, 200_000)
        val next = try {
            ScreenEncoder(codec, w, h, fps, kbps) { data, len, key, capture, enc -> s.sendVideo(data, len, key, capture, enc) }
        } catch (e: Exception) {
            update(current.copy(detail = "El codificador del teléfono falló: ${e.message}"))
            return
        }
        synchronized(encoderLock) {
            val old = encoder
            d.resize(w, h, dpi)
            d.surface = next.surface
            encoder = next
            old?.release()
        }
        val audioOn = req.audio && audioAvailable()
        s.streamStarted(codec, w, h, fps, kbps, if (audioOn) 48_000 else 0, if (audioOn) 2 else 0, "MediaCodec ${next.name}")
        next.requestKeyframe()
        if (audioOn) startAudio() else stopAudio()
        update(current.copy(detail = "${codec.label} ${w}x$h @ $fps fps · $kbps kbps"))
    }

    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) {}
        override fun onDisplayRemoved(displayId: Int) {}
        override fun onDisplayChanged(displayId: Int) {
            // Giró el teléfono: el stream se vuelve a armar con la nueva orientación.
            val req = lastRequest ?: return
            val client = lastClient ?: return
            val e = encoder ?: return
            val (sw, sh, _) = screen()
            val (w, h) = ScreenEncoder.fitSize(e.codec, sw, sh, req.maxWidth, req.maxHeight)
            if (w != e.width || h != e.height) configure(client, req)
        }
    }

    private fun screen(): Triple<Int, Int, Int> {
        val m = DisplayMetrics()
        @Suppress("DEPRECATION")
        (getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.getRealMetrics(m)
        return Triple(m.widthPixels, m.heightPixels, m.densityDpi)
    }

    private fun refreshHz(): Int {
        @Suppress("DEPRECATION")
        val hz = (getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.refreshRate
        return Math.round(hz).coerceIn(30, 240)
    }

    // --------------------------------------------------------------------------------------- audio (Android 10+)

    private fun audioAvailable() = Build.VERSION.SDK_INT >= 29 &&
        checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    private fun startAudio() {
        if (audioRunning || Build.VERSION.SDK_INT < 29) return
        val p = projection ?: return
        val record = try {
            val config = AudioPlaybackCaptureConfiguration.Builder(p)
                .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                .addMatchingUsage(AudioAttributes.USAGE_GAME)
                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                .build()
            val format = AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT).setSampleRate(48_000)
                .setChannelMask(AudioFormat.CHANNEL_IN_STEREO).build()
            val min = AudioRecord.getMinBufferSize(48_000, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT)
            @Suppress("MissingPermission")
            AudioRecord.Builder().setAudioFormat(format).setBufferSizeInBytes(maxOf(min, 48_000 * 4 / 10))
                .setAudioPlaybackCaptureConfig(config).build()
        } catch (_: Exception) {
            return
        }
        if (record.state != AudioRecord.STATE_INITIALIZED) {
            record.release()
            return
        }
        audio = record
        audioRunning = true
        record.startRecording()
        Thread({
            val buf = ShortArray(480 * 2)  // 10 ms estéreo
            while (audioRunning) {
                val n = record.read(buf, 0, buf.size)
                if (n > 0) server?.sendAudio(buf, n / 2, 48_000, 2)
                else if (n < 0) break
            }
        }, "lancast-share-audio").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    private fun stopAudio() {
        audioRunning = false
        audio?.let {
            runCatching { it.stop() }
            it.release()
        }
        audio = null
    }

    // --------------------------------------------------------------------------------------- notificación y estado

    private fun startForegroundCompat(n: Notification) {
        if (Build.VERSION.SDK_INT >= 29) startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        else startForeground(NOTIFICATION_ID, n)
    }

    private fun notify(n: Notification) {
        runCatching { (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).notify(NOTIFICATION_ID, n) }
    }

    private fun notification(title: String, text: String?): Notification {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(NotificationChannel(CHANNEL, "Compartir pantalla", NotificationManager.IMPORTANCE_HIGH))
        }
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 1, Intent(this, ShareService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_menu_share)
            .setContentTitle(title)
            .setContentText(text ?: "LanCast está compartiendo esta pantalla")
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, "Dejar de compartir", stop).build())
            .build()
    }

    private fun update(s: State) {
        if (s.detail != current.detail && s.detail.isNotEmpty()) AppLog.add("Compartir: ${s.detail}")
        current = s
        ui.post { listeners.toList().forEach { it(s) } }
    }

    // Puntero de flecha (BGRA) para el cliente de PC.
    private fun arrow(): ByteArray {
        val px = ByteArray(ARROW_W * ARROW_H * 4)
        for (y in 0 until ARROW_H) for (x in 0 until ARROW_W) {
            val inside = x <= y * 0.55 && y < ARROW_H - 4 || (y >= ARROW_H - 8 && x in (y - ARROW_H + 8)..(y - ARROW_H + 11) && x < ARROW_W)
            val edge = inside && (x == 0 || x >= y * 0.55 - 1.2 || y >= ARROW_H - 5)
            if (!inside) continue
            val i = (y * ARROW_W + x) * 4
            val v = if (edge) 0 else 255
            px[i] = v.toByte()
            px[i + 1] = v.toByte()
            px[i + 2] = v.toByte()
            px[i + 3] = 255.toByte()
        }
        return px
    }

    companion object {
        const val EXTRA_CODE = "code"
        const val EXTRA_DATA = "data"
        private const val ACTION_STOP = "com.lancast.android.STOP_SHARE"
        private const val CHANNEL = "share"
        private const val NOTIFICATION_ID = 7
        private const val ARROW_W = 16
        private const val ARROW_H = 24

        @Volatile var current = State()
            private set
        private val listeners = mutableListOf<(State) -> Unit>()

        fun addListener(l: (State) -> Unit) {
            listeners += l
            l(current)
        }

        fun removeListener(l: (State) -> Unit) {
            listeners -= l
        }

        fun stop(context: Context) {
            context.startService(Intent(context, ShareService::class.java).setAction(ACTION_STOP))
        }
    }
}
