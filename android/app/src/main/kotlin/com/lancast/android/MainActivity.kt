package com.lancast.android

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
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
    private val ui = Handler(Looper.getMainLooper())
    @Volatile private var scanning = false
    @Volatile private var scanGeneration = 0
    private var found: List<DiscoveredHost> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        storage = Storage(this)
        setContentView(build())
    }

    override fun onResume() {
        super.onResume()
        refreshPaired()
        scanning = true
        scanLoop()
    }

    override fun onPause() {
        super.onPause()
        scanning = false
    }

    private fun build(): View {
        val col = column(20)

        col.addView(text("LanCast", 30f, bold = true))
        nameLabel = text("", 14f, Ui.MUTED)
        col.addView(nameLabel)
        updateName()

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

        col.addView(section("Ajustes"))
        col.addView(option("Resolución", listOf("screen" to "Igual a esta pantalla", "720" to "720p", "1080" to "1080p", "1440" to "1440p"),
            { storage.resolution }, { storage.resolution = it }))
        col.addView(option("FPS", listOf("0" to "Igual a esta pantalla", "30" to "30", "60" to "60", "90" to "90", "120" to "120"),
            { storage.fps.toString() }, { storage.fps = it.toInt() }))
        col.addView(option("Códec", listOf("auto" to "Automático", "hevc" to "HEVC", "h264" to "H.264", "av1" to "AV1"),
            { storage.codec }, { storage.codec = it }))
        col.addView(option("Audio del PC", listOf("true" to "Sí", "false" to "No"), { storage.audio.toString() }, { storage.audio = it == "true" }))
        col.addView(option("Control táctil", listOf("true" to "Touchpad", "false" to "Directo"), { storage.touchpad.toString() },
            { storage.touchpad = it == "true" }))
        col.addView(button("Nombre de este teléfono") { editName() }.apply { margins(top = dp(8)) })

        col.addView(text(
            "Necesitas LanCast 1.5 o superior en el PC y estar en la misma red. La primera vez el PC muestra un PIN de 6 dígitos.\n\n" +
                "Gestos: un dedo mueve el cursor · toque = clic · dos dedos = clic derecho / rueda · pellizcar = zoom · " +
                "mantener = arrastrar · tres dedos = teclado.",
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
            pairedBox.addView(card(p.name, p.address, "Olvidar", {
                AlertDialog.Builder(this).setMessage("¿Olvidar ${p.name}? La próxima vez pedirá PIN.")
                    .setPositiveButton("Olvidar") { _, _ ->
                        storage.forget(p.id)
                        refreshPaired()
                    }.setNegativeButton("Cancelar", null).show()
            }) { if (p.address.isNotBlank()) open(p.address, Proto.DEFAULT_CONTROL_PORT) })
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
}
