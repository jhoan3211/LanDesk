package com.lancast.android

import android.app.Activity
import android.Manifest
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import com.lancast.android.Ui.button
import com.lancast.android.Ui.column
import com.lancast.android.Ui.dp
import com.lancast.android.Ui.lp
import com.lancast.android.Ui.row
import com.lancast.android.Ui.text
import com.lancast.core.DiscoveredHost
import com.lancast.core.Proto
import com.lancast.core.Session
import kotlin.concurrent.thread

class MainActivity : Activity() {
    private lateinit var storage: Storage
    private lateinit var hostsBox: LinearLayout
    private lateinit var pairedBox: LinearLayout
    private lateinit var scanStatus: TextView
    private lateinit var nameLabel: TextView
    private lateinit var updateBox: LinearLayout
    private lateinit var shareStatus: TextView
    private lateinit var shareButton: TextView
    private lateinit var controlButton: TextView
    private lateinit var clientsBox: LinearLayout
    private var pinDialog: AlertDialog? = null
    private var shownPin: String? = null
    private val shareListener: (ShareService.State) -> Unit = { showShare(it) }
    private val ui = Handler(Looper.getMainLooper())
    @Volatile private var scanning = false
    @Volatile private var scanGeneration = 0
    private var found: List<DiscoveredHost> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        storage = Storage(this)
        WifiBinding.start(this)
        setContentView(build())
    }

    // ------------------------------------------------------------------------------------------- actualización

    // Automática: al abrir la app, como mucho cada 6 h. Manual: botón "Buscar actualizaciones" (avisa el resultado).
    private fun checkUpdate(manual: Boolean) {
        val now = System.currentTimeMillis()
        if (!manual && now - storage.lastUpdateCheck < 6 * 3600_000L) return
        if (manual) Toast.makeText(this, "Buscando actualizaciones…", Toast.LENGTH_SHORT).show()
        thread(name = "lancast-update", isDaemon = true) {
            val release = try {
                Updater.check(this)
            } catch (_: Exception) {
                // Sin internet: la automática se intenta en otro momento.
                if (manual) ui.post { Toast.makeText(this, "No se pudo consultar GitHub (¿hay internet?)", Toast.LENGTH_LONG).show() }
                return@thread
            }
            storage.lastUpdateCheck = now
            ui.post {
                if (release != null) {
                    showUpdate(release)
                } else if (manual) {
                    Toast.makeText(this, "Ya tienes la última versión (${Updater.currentVersion(this)})", Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    private fun showUpdate(release: Updater.Release) {
        updateBox.removeAllViews()
        updateBox.addView(card("Nueva versión ${release.version}", "Tienes la ${Updater.currentVersion(this)}. La descarga se verifica con SHA-256.",
            "Actualizar", { installUpdate(release) }, null))
    }

    private fun installUpdate(release: Updater.Release) {
        if (!Updater.canInstall(this)) {
            AlertDialog.Builder(this).setTitle("Permitir actualizaciones")
                .setMessage("Android pide permitir que LanCast instale su propia actualización. Activa \"Permitir de esta fuente\" y vuelve.")
                .setPositiveButton("Abrir ajustes") { _, _ ->
                    if (!Updater.openInstallPermission(this)) {
                        AlertDialog.Builder(this).setMessage("Abre Ajustes → Apps → Acceso especial → Instalar apps desconocidas → " +
                            "LanCast y actívalo. Luego vuelve y toca Actualizar.").setPositiveButton("Entendido", null).show()
                    }
                }
                .setNegativeButton("Cancelar", null).show()
            return
        }
        val status = text("Descargando…", 14f)
        val dialog = AlertDialog.Builder(this).setTitle("LanCast ${release.version}").setView(column(20).apply { addView(status) })
            .setCancelable(false).show()
        thread(name = "lancast-update-download", isDaemon = true) {
            try {
                val apk = Updater.download(this, release) { pct -> ui.post { status.text = "Descargando… $pct %" } }
                ui.post {
                    dialog.dismiss()
                    Updater.install(this, apk) { err -> ui.post { Toast.makeText(this, err, Toast.LENGTH_LONG).show() } }
                }
            } catch (e: Exception) {
                ui.post {
                    dialog.dismiss()
                    Toast.makeText(this, "No se pudo actualizar: ${e.message ?: e.javaClass.simpleName}", Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        checkUpdate(manual = false)
        refreshPaired()
        ShareService.addListener(shareListener)
        refreshClients()
        scanning = true
        scanLoop()
    }

    override fun onPause() {
        super.onPause()
        scanning = false
        ShareService.removeListener(shareListener)
    }

    // ------------------------------------------------------------------------------------------- compartir este teléfono

    private fun share() {
        if (ShareService.current.sharing) {
            ShareService.stop(this)
            return
        }
        // Audio de las apps (Android 10+) y el aviso con el PIN; si se niegan, igual se comparte la imagen.
        val wanted = buildList {
            if (Build.VERSION.SDK_INT >= 29 && checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                add(Manifest.permission.RECORD_AUDIO)
            }
            if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                add(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
        if (wanted.isNotEmpty()) {
            requestPermissions(wanted.toTypedArray(), REQUEST_PERMISSIONS)
            return
        }
        askCapture()
    }

    @Deprecated("API clásica de permisos: suficiente aquí")
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        @Suppress("DEPRECATION")
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_PERMISSIONS) askCapture()
    }

    private fun askCapture() {
        val mpm = getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        @Suppress("DEPRECATION")
        startActivityForResult(mpm.createScreenCaptureIntent(), REQUEST_CAPTURE)
    }

    @Deprecated("API de actividad clásica: suficiente aquí")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_CAPTURE) return
        if (resultCode != RESULT_OK || data == null) {
            Toast.makeText(this, "Sin permiso de captura no se puede compartir la pantalla", Toast.LENGTH_LONG).show()
            return
        }
        startForegroundService(Intent(this, ShareService::class.java)
            .putExtra(ShareService.EXTRA_CODE, resultCode).putExtra(ShareService.EXTRA_DATA, data))
        if (!ControlService.enabled()) Toast.makeText(this, "Para que te controlen, activa también el control remoto", Toast.LENGTH_LONG).show()
    }

    private fun showShare(s: ShareService.State) {
        if (!::shareStatus.isInitialized) return
        shareButton.text = if (s.sharing) "Dejar de compartir" else "Compartir pantalla"
        shareButton.background = Ui.rounded(if (s.sharing) Ui.DANGER else Ui.ACCENT, dp(10).toFloat())
        val ip = DeviceInfo.localIp()
        shareStatus.text = when {
            !s.sharing -> "Otro teléfono o un PC con LanCast podrán ver y controlar este teléfono." +
                (if (s.detail.isNotEmpty()) "\n${s.detail}" else "")
            s.client != null -> "${s.client} está viendo esta pantalla.\n${s.detail}"
            else -> "Esperando conexión. En el otro equipo, búscalo en «Equipos en la red» o escribe ${ip ?: "la IP de este teléfono"}."
        }
        controlButton.text = if (ControlService.enabled()) "Control remoto: activado" else "Control remoto: desactivado (tocar para activar)"
        if (s.pin != null && s.pin != shownPin) {
            shownPin = s.pin
            pinDialog?.dismiss()
            pinDialog = AlertDialog.Builder(this).setTitle("PIN para ${s.pinFor}")
                .setView(text(s.pin, 44f, Ui.TEXT, bold = true).apply {
                    gravity = Gravity.CENTER
                    setPadding(0, dp(16), 0, dp(16))
                    letterSpacing = 0.15f
                })
                .setMessage("Escríbelo en el otro equipo para emparejarlo con este teléfono.")
                .setPositiveButton("Cerrar", null).show()
        } else if (s.pin == null) {
            shownPin = null
            pinDialog?.dismiss()
            pinDialog = null
            refreshClients()
        }
    }

    // El control necesita el servicio de accesibilidad de LanCast (Android no deja tocar la pantalla de otra forma).
    private fun enableControl() {
        val restricted = if (Build.VERSION.SDK_INT >= 33) "\n\nSi Android dice que es un «ajuste restringido»: abre «Info de la app» → menú ⋮ " +
            "→ «Permitir ajustes restringidos» y vuelve a intentarlo (pasa con apps instaladas fuera de la tienda)." else ""
        AlertDialog.Builder(this).setTitle("Control remoto")
            .setMessage("Para que otro equipo pueda tocar y escribir en este teléfono, activa «LanCast: control remoto» en " +
                "Ajustes → Accesibilidad → Apps instaladas. Solo actúa mientras compartes la pantalla y con equipos emparejados con el PIN." +
                restricted)
            .setPositiveButton("Abrir Accesibilidad") { _, _ -> startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)) }
            .setNeutralButton("Info de la app") { _, _ ->
                startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName")))
            }
            .setNegativeButton("Cancelar", null).show()
    }

    private fun refreshClients() {
        if (!::clientsBox.isInitialized) return
        clientsBox.removeAllViews()
        for (c in storage.clients()) {
            clientsBox.addView(card(c.name, c.address + " · puede conectarse sin PIN", "Olvidar", {
                AlertDialog.Builder(this).setMessage("¿Olvidar ${c.name}? La próxima vez tendrá que escribir el PIN.")
                    .setPositiveButton("Olvidar") { _, _ ->
                        storage.forgetClient(c.id)
                        refreshClients()
                    }.setNegativeButton("Cancelar", null).show()
            }, null))
        }
    }

    private fun build(): View {
        val col = column(20)

        col.addView(text("LanCast", 30f, bold = true))
        nameLabel = text("", 14f, Ui.MUTED)
        col.addView(nameLabel)
        updateName()
        updateBox = column()
        col.addView(updateBox)

        col.addView(section("Equipos en la red"))
        scanStatus = text("Buscando PC con LanCast…", 13f, Ui.MUTED)
        col.addView(scanStatus)
        hostsBox = column()
        col.addView(hostsBox)

        col.addView(section("Conectar por IP"))
        val ipRow = row()
        val ip = EditText(this).apply {
            hint = "192.168.40.7"
            setText(storage.recent.firstOrNull() ?: "")
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            imeOptions = EditorInfo.IME_ACTION_GO
            setTextColor(Ui.TEXT)
            setHintTextColor(Ui.MUTED)
            setOnEditorActionListener { v, _, _ ->
                connect(v.text.toString())
                true
            }
        }
        ipRow.addView(ip, lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        ipRow.addView(button("Conectar", primary = true) { connect(ip.text.toString()) }, lp(ViewGroup.LayoutParams.WRAP_CONTENT))
        col.addView(ipRow)
        val recentRow = row()
        storage.recent.take(4).forEach { addr ->
            recentRow.addView(button(addr) { connect(addr) }.apply { textSize = 13f }, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { marginEnd = dp(6) })
        }
        if (storage.recent.isNotEmpty()) col.addView(recentRow)

        col.addView(section("Equipos emparejados"))
        pairedBox = column()
        col.addView(pairedBox)

        col.addView(section("Compartir este teléfono"))
        shareStatus = text("", 13f, Ui.MUTED)
        col.addView(shareStatus)
        val shareRow = row().apply { setPadding(0, dp(8), 0, dp(4)) }
        shareButton = button("Compartir pantalla", primary = true) { share() }
        shareRow.addView(shareButton, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            marginEnd = dp(8)
        })
        col.addView(shareRow)
        controlButton = button("Control remoto") { enableControl() }.apply { textSize = 13f }
        col.addView(controlButton)
        clientsBox = column()
        col.addView(clientsBox)

        col.addView(section("Ajustes"))
        col.addView(option("Resolución", listOf("screen" to "Igual a esta pantalla", "720" to "720p", "1080" to "1080p", "1440" to "1440p"),
            { storage.resolution }, { storage.resolution = it }))
        col.addView(option("FPS", listOf("0" to "Igual a esta pantalla", "30" to "30", "60" to "60", "90" to "90", "120" to "120"),
            { storage.fps.toString() }, { storage.fps = it.toInt() }))
        col.addView(option("Bitrate (Wi-Fi)", listOf("40" to "40 Mbps", "60" to "60 Mbps", "100" to "100 Mbps", "0" to "Automático", "20" to "20 Mbps"),
            { storage.bitrateMbps.toString() }, { storage.bitrateMbps = it.toInt() }))
        col.addView(option("Códec", listOf("auto" to "Automático", "hevc" to "HEVC", "h264" to "H.264", "av1" to "AV1"),
            { storage.codec }, { storage.codec = it }))
        col.addView(option("Audio del PC", listOf("true" to "Sí", "false" to "No"), { storage.audio.toString() }, { storage.audio = it == "true" }))
        col.addView(option("Control táctil", listOf("true" to "Touchpad", "false" to "Directo"), { storage.touchpad.toString() },
            { storage.touchpad = it == "true" }))
        col.addView(button("Nombre de este teléfono") { editName() }.apply { margins(top = dp(8)) })
        col.addView(button("Buscar actualizaciones (versión ${Updater.currentVersion(this)})") { checkUpdate(manual = true) }
            .apply { margins(top = dp(8)) })

        col.addView(text(
            "Necesitas LanCast 1.5 o superior en el PC y estar en la misma red. La primera vez el PC muestra un PIN de 6 dígitos.\n\n" +
                "Gestos: un dedo mueve el cursor · toque = clic · dos dedos = clic derecho / rueda · pellizcar = zoom · " +
                "tres dedos arrastrando = mover la vista con zoom · mantener = arrastrar · tocar con tres dedos = teclado.\n\n" +
                "También funcionan el S Pen, un mouse o teclado Bluetooth y los mandos (en el PC aparecen como mando de Xbox; " +
                "requiere el driver ViGEmBus 1.22.0 en el PC). Para despertar un PC dormido, toca ⋯ en Equipos emparejados.",
            12f, Ui.MUTED).apply { margins(top = dp(20)) })

        return ScrollView(this).apply {
            setBackgroundColor(Ui.BG)
            addView(col)
        }
    }

    private fun View.margins(top: Int = 0): View {
        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = top }
        return this
    }

    private fun section(title: String) = text(title, 13f, Ui.ACCENT, bold = true).apply {
        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(22)
            bottomMargin = dp(6)
        }
    }

    // Fila "Etiqueta ........ [Valor]": tocar el valor recorre las opciones.
    private fun option(label: String, choices: List<Pair<String, String>>, get: () -> String, set: (String) -> Unit): View {
        val r = row().apply { setPadding(0, dp(4), 0, dp(4)) }
        r.addView(text(label, 15f), lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        lateinit var value: TextView
        fun current() = choices.firstOrNull { it.first == get() }?.second ?: choices.first().second
        value = button(current()) {
            val i = choices.indexOfFirst { it.first == get() }
            set(choices[(i + 1) % choices.size].first)
            value.text = current()
        }.apply { textSize = 14f }
        r.addView(value, lp(ViewGroup.LayoutParams.WRAP_CONTENT))
        return r
    }

    private fun card(title: String, subtitle: String, action: String?, onAction: (() -> Unit)?, onClick: (() -> Unit)?): View {
        val r = row().apply {
            setPadding(dp(14), dp(12), dp(10), dp(12))
            background = Ui.rounded(Ui.PANEL, dp(12).toFloat())
            if (onClick != null) setOnClickListener { onClick() }
        }
        val texts = column()
        texts.addView(text(title, 16f, bold = true))
        texts.addView(text(subtitle, 13f, Ui.MUTED))
        r.addView(texts, lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        if (action != null && onAction != null) r.addView(button(action) { onAction() }.apply { textSize = 13f }, lp(ViewGroup.LayoutParams.WRAP_CONTENT))
        return r.apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dp(8) }
        }
    }

    private fun scanLoop() {
        val generation = ++scanGeneration  // Un solo ciclo de búsqueda aunque se vuelva rápido a la pantalla.
        thread(name = "lancast-discovery", isDaemon = true) {
            while (scanning && generation == scanGeneration) {
                val hosts = try {
                    Session.discover(1200, DeviceInfo.broadcastAddresses())
                } catch (_: Exception) {
                    emptyList()
                }
                ui.post { showHosts(hosts) }
                Thread.sleep(2500)
            }
        }
    }

    private fun showHosts(hosts: List<DiscoveredHost>) {
        if (hosts == found && hostsBox.childCount > 0) return
        found = hosts
        hostsBox.removeAllViews()
        val paired = storage.paired().map { it.id }.toSet()
        scanStatus.text = if (hosts.isEmpty()) {
            "No se encontró ningún PC. Abre LanCast en el PC y revisa que el teléfono esté en el mismo Wi-Fi (misma red que los PC)."
        } else {
            "Toca un equipo para conectarte."
        }
        for (h in hosts) {
            val sub = buildString {
                append(h.address)
                if (h.deviceId in paired) append(" · emparejado")
                if (h.busy) append(" · en uso")
            }
            hostsBox.addView(card(h.name, sub, null, null) { open(h.address, h.controlPort) })
        }
    }

    private fun refreshPaired() {
        pairedBox.removeAllViews()
        val paired = storage.paired()
        if (paired.isEmpty()) pairedBox.addView(text("Ninguno todavía.", 13f, Ui.MUTED))
        for (p in paired) {
            val mac = storage.mac(p.address)
            pairedBox.addView(card(p.name, p.address, "⋯", {
                val actions = buildList {
                    if (mac != null) add("Despertar (Wake-on-LAN)" to { wake(p.name, p.address, mac) })
                    add("Olvidar" to {
                        AlertDialog.Builder(this@MainActivity).setMessage("¿Olvidar ${p.name}? La próxima vez pedirá PIN.")
                            .setPositiveButton("Olvidar") { _, _ ->
                                storage.forget(p.id)
                                refreshPaired()
                            }.setNegativeButton("Cancelar", null).show()
                    })
                }
                AlertDialog.Builder(this).setTitle(p.name).setItems(actions.map { it.first }.toTypedArray()) { _, i -> actions[i].second() }.show()
            }) { if (p.address.isNotBlank()) open(p.address, Proto.DEFAULT_CONTROL_PORT) })
        }
    }

    // Enciende un PC suspendido o apagado (necesita Wake-on-LAN activado en la BIOS y en la tarjeta de red).
    private fun wake(name: String, address: String, mac: ByteArray) {
        thread(name = "lancast-wol", isDaemon = true) {
            val ok = runCatching { WakeOnLan.send(mac, address) }.isSuccess
            ui.post {
                Toast.makeText(this, if (ok) "Despertando $name… suele tardar 10-30 s en aparecer" else "No se pudo enviar el aviso",
                    Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun connect(text: String) {
        val t = text.trim()
        if (t.isEmpty()) return
        val host = t.substringBefore(':')
        val port = t.substringAfter(':', "").toIntOrNull() ?: Proto.DEFAULT_CONTROL_PORT
        open(host, port)
    }

    private fun open(address: String, port: Int) {
        startActivity(Intent(this, StreamActivity::class.java).putExtra(StreamActivity.EXTRA_ADDRESS, address).putExtra(StreamActivity.EXTRA_PORT, port))
    }

    private fun updateName() {
        nameLabel.text = "Este teléfono: ${storage.deviceName}"
    }

    private fun editName() {
        val input = EditText(this).apply {
            setText(storage.deviceName)
            gravity = Gravity.CENTER_VERTICAL
        }
        AlertDialog.Builder(this).setTitle("Nombre de este teléfono").setMessage("Así lo verá el PC al conectarse.")
            .setView(input)
            .setPositiveButton("Guardar") { _, _ ->
                storage.deviceName = input.text.toString()
                updateName()
            }.setNegativeButton("Cancelar", null).show()
    }

    companion object {
        private const val REQUEST_CAPTURE = 21
        private const val REQUEST_PERMISSIONS = 22
    }
}
