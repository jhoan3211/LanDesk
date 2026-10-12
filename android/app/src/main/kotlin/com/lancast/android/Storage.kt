package com.lancast.android

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import android.provider.Settings
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import com.lancast.core.Crypto
import com.lancast.core.PairingStore
import com.lancast.core.toHex
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

// Preferencias de la app y equipos emparejados. Los secretos de emparejamiento se cifran con una clave del
// Android Keystore (no sale del hardware seguro del teléfono).
class Storage(context: Context) : PairingStore {
    private val prefs: SharedPreferences = context.getSharedPreferences("lancast", Context.MODE_PRIVATE)
    private val pairings: SharedPreferences = context.getSharedPreferences("pairings", Context.MODE_PRIVATE)
    private val appContext = context.applicationContext

    val deviceId: ByteArray
        get() {
            val hex = prefs.getString("device_id", null)
            if (hex != null && hex.length == 32) return hex.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
            val id = Crypto.randomBytes(16)
            prefs.edit().putString("device_id", id.toHex()).apply()
            return id
        }

    var deviceName: String
        get() = prefs.getString("device_name", null) ?: defaultName()
        set(v) = prefs.edit().putString("device_name", v.trim().ifEmpty { defaultName() }).apply()

    var resolution: String  // "screen", "720", "1080", "1440"
        get() = prefs.getString("resolution", "screen")!!
        set(v) = prefs.edit().putString("resolution", v).apply()

    var fps: Int  // 0 = igual a la pantalla
        get() = prefs.getInt("fps", 0)
        set(v) = prefs.edit().putInt("fps", v).apply()

    // Bitrate fijo en Mbps (0 = automático del host). Por Wi-Fi conviene limitarlo: el automático para 1440p a
    // 120 fps pasa de 150 Mbps y las ráfagas llenan los búferes del router y del Wi-Fi (pérdidas y cortes).
    var bitrateMbps: Int
        get() = prefs.getInt("bitrate", 40)
        set(v) = prefs.edit().putInt("bitrate", v).apply()

    var codec: String  // "auto", "h264", "hevc", "av1"
        get() = prefs.getString("codec", "auto")!!
        set(v) = prefs.edit().putString("codec", v).apply()

    var audio: Boolean
        get() = prefs.getBoolean("audio", true)
        set(v) = prefs.edit().putBoolean("audio", v).apply()

    var touchpad: Boolean  // true = el dedo mueve el cursor; false = toque directo
        get() = prefs.getBoolean("touchpad", true)
        set(v) = prefs.edit().putBoolean("touchpad", v).apply()

    var showStats: Boolean
        get() = prefs.getBoolean("stats", false)
        set(v) = prefs.edit().putBoolean("stats", v).apply()

    var recent: List<String>
        get() = prefs.getString("recent", "")!!.split('\n').filter { it.isNotBlank() }
        set(v) = prefs.edit().putString("recent", v.distinct().take(6).joinToString("\n")).apply()

    fun addRecent(address: String) {
        recent = listOf(address) + recent.filter { it != address }
    }

    data class Paired(val id: String, val name: String, val address: String)

    fun paired(): List<Paired> = pairings.all.keys.filter { it.endsWith(".name") }.map { key ->
        val id = key.removeSuffix(".name")
        Paired(id, pairings.getString("$id.name", "") ?: "", pairings.getString("$id.address", "") ?: "")
    }

    // --- PairingStore ---

    override fun secret(hostId: String): ByteArray? {
        val blob = pairings.getString("$hostId.secret", null) ?: return null
        return try {
            val raw = Base64.decode(blob, Base64.NO_WRAP)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, raw.copyOfRange(0, 12)))
            cipher.doFinal(raw, 12, raw.size - 12)
        } catch (_: Exception) {
            null
        }
    }

    override fun save(hostId: String, name: String, address: String, secret: ByteArray) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key())
        val blob = cipher.iv + cipher.doFinal(secret)
        pairings.edit()
            .putString("$hostId.secret", Base64.encodeToString(blob, Base64.NO_WRAP))
            .putString("$hostId.name", name)
            .putString("$hostId.address", address)
            .apply()
    }

    override fun forget(hostId: String) {
        pairings.edit().remove("$hostId.secret").remove("$hostId.name").remove("$hostId.address").apply()
    }

    private fun key(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getEntry(KEY_ALIAS, null) as? KeyStore.SecretKeyEntry)?.let { return it.secretKey }
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        gen.init(
            KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build()
        )
        return gen.generateKey()
    }

    private fun defaultName(): String {
        val name = Settings.Global.getString(appContext.contentResolver, Settings.Global.DEVICE_NAME)
        if (!name.isNullOrBlank()) return name
        val model = Build.MODEL ?: "Android"
        val maker = Build.MANUFACTURER ?: ""
        return if (model.startsWith(maker, ignoreCase = true)) model else "$maker $model".trim()
    }

    companion object {
        private const val KEY_ALIAS = "lancast-pairing"
    }
}
