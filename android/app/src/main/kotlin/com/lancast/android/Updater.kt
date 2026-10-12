package com.lancast.android

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.provider.Settings
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

// Actualización de la app desde las versiones publicadas en GitHub: descarga LanCast.apk, comprueba su SHA-256 con
// el de las notas de la versión y la instala con PackageInstaller (Android pide confirmación y exige la misma firma).
object Updater {
    private const val API = "https://api.github.com/repos/jhoan3211/LanDesk/releases/latest"
    private const val ACTION_STATUS = "com.lancast.android.UPDATE_STATUS"

    data class Release(val version: String, val apkUrl: String, val sha256: String, val notes: String)

    fun currentVersion(context: Context): String =
        context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "0"

    private fun open(context: Context, url: String): HttpURLConnection {
        val u = URL(url)
        val conn = (WifiBinding.internet(context)?.openConnection(u) ?: u.openConnection()) as HttpURLConnection
        conn.connectTimeout = 8000
        conn.readTimeout = 20000
        conn.setRequestProperty("User-Agent", "LanCast-Android")
        return conn
    }

    // Bloquea. null = no hay versión nueva (o no se pudo consultar).
    fun check(context: Context): Release? {
        val body = open(context, API).run {
            setRequestProperty("Accept", "application/vnd.github+json")
            if (responseCode != 200) return null
            inputStream.bufferedReader().use { it.readText() }
        }
        val tag = Regex("\"tag_name\"\\s*:\\s*\"v?([^\"]+)\"").find(body)?.groupValues?.get(1) ?: return null
        val apk = Regex("\"browser_download_url\"\\s*:\\s*\"([^\"]+LanCast\\.apk)\"").find(body)?.groupValues?.get(1) ?: return null
        val notes = Regex("\"body\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"").find(body)?.groupValues?.get(1)
            ?.replace("\\r", "")?.replace("\\n", "\n")?.replace("\\\"", "\"") ?: ""
        // Línea en las notas: "SHA-256 de LanCast.apk: <64 hex>".
        val sha = Regex("LanCast\\.apk[^0-9a-fA-F\\n]*([0-9a-fA-F]{64})").find(notes)?.groupValues?.get(1)?.lowercase() ?: return null
        if (!newer(tag, currentVersion(context))) return null
        return Release(tag, apk, sha, notes)
    }

    fun newer(remote: String, local: String): Boolean {
        fun parts(v: String) = v.split('.', '-').mapNotNull { it.toIntOrNull() }
        val a = parts(remote)
        val b = parts(local)
        for (i in 0 until maxOf(a.size, b.size)) {
            val x = a.getOrElse(i) { 0 }
            val y = b.getOrElse(i) { 0 }
            if (x != y) return x > y
        }
        return false
    }

    // Bloquea. Devuelve el archivo verificado o lanza una excepción con el motivo.
    fun download(context: Context, release: Release, progress: (Int) -> Unit): File {
        val file = File(context.cacheDir, "LanCast-${release.version}.apk")
        val digest = MessageDigest.getInstance("SHA-256")
        open(context, release.apkUrl).run {
            instanceFollowRedirects = true
            if (responseCode != 200) throw IllegalStateException("GitHub respondió $responseCode")
            val total = contentLengthLong
            inputStream.use { input ->
                file.outputStream().use { out ->
                    val buf = ByteArray(64 * 1024)
                    var done = 0L
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        digest.update(buf, 0, n)
                        done += n
                        if (total > 0) progress((done * 100 / total).toInt())
                    }
                }
            }
        }
        val hex = digest.digest().joinToString("") { "%02x".format(it) }
        if (hex != release.sha256) {
            file.delete()
            throw SecurityException("La descarga no coincide con el SHA-256 publicado: descartada")
        }
        return file
    }

    // Android 8+: la app necesita el permiso "Instalar apps desconocidas" (una sola vez).
    fun canInstall(context: Context) = context.packageManager.canRequestPackageInstalls()

    // Algunos teléfonos no tienen esa pantalla de ajustes: se abre la de la app y, si tampoco, se explica dónde está.
    fun openInstallPermission(activity: Activity): Boolean {
        val pkg = Uri.parse("package:${activity.packageName}")
        for (intent in listOf(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, pkg), Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, pkg))) {
            try {
                activity.startActivity(intent)
                return true
            } catch (_: android.content.ActivityNotFoundException) {
            }
        }
        return false
    }

    fun install(activity: Activity, apk: File, onError: (String) -> Unit) {
        val installer = activity.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(activity.packageName)
            if (Build.VERSION.SDK_INT >= 31) setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        }
        val id = installer.createSession(params)
        installer.openSession(id).use { session ->
            session.openWrite("LanCast.apk", 0, apk.length()).use { out ->
                apk.inputStream().use { it.copyTo(out) }
                session.fsync(out)
            }
            val receiver = object : BroadcastReceiver() {
                override fun onReceive(context: Context, intent: Intent) {
                    when (intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
                        PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                            @Suppress("DEPRECATION")
                            val confirm = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                            else intent.getParcelableExtra(Intent.EXTRA_INTENT)
                            confirm?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)?.let { context.startActivity(it) }
                        }
                        PackageInstaller.STATUS_SUCCESS -> context.unregisterReceiver(this)
                        else -> {
                            context.unregisterReceiver(this)
                            onError(intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "La instalación no se completó")
                        }
                    }
                }
            }
            val filter = IntentFilter(ACTION_STATUS)
            // Solo lo envía PackageInstaller con nuestro PendingIntent: no se exporta (la constante vale en Android 8+).
            @android.annotation.SuppressLint("InlinedApi")
            val notExported = Context.RECEIVER_NOT_EXPORTED
            activity.applicationContext.registerReceiver(receiver, filter, notExported)
            val intent = Intent(ACTION_STATUS).setPackage(activity.packageName)
            val pending = PendingIntent.getBroadcast(activity, id, intent, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE)
            session.commit(pending.intentSender)
        }
    }
}
