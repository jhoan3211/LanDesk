package com.lancast.android

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import com.lancast.core.FileSink
import java.io.File
import java.io.OutputStream

// Archivos que manda el PC: en Descargas/LanCast (visible en la app Archivos). En Android 8-9, en la carpeta de la
// app (Android/data/com.lancast.android/files/Download/LanCast), que no pide permisos.
class FileSaver(context: Context) : FileSink {
    private val app = context.applicationContext
    private val pending = HashMap<String, Any>()  // nombre -> Uri (MediaStore) o File

    // Partes de la ruta seguras: sin "..", sin caracteres raros.
    private fun clean(name: String): Pair<String, String> {
        val parts = name.split('/', '\\').map { p -> p.filter { it >= ' ' && it !in "<>:\"|?*" }.trim().trimEnd('.') }
            .filter { it.isNotEmpty() && it != "." && it != ".." }
        val file = parts.lastOrNull() ?: "archivo"
        val dirs = parts.dropLast(1).joinToString("/")
        return dirs to file
    }

    override fun open(name: String, size: Long): OutputStream? {
        val (dirs, file) = clean(name)
        val sub = if (dirs.isEmpty()) "LanCast" else "LanCast/$dirs"
        return if (Build.VERSION.SDK_INT >= 29) {
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, file)
                put(MediaStore.Downloads.RELATIVE_PATH, "${Environment.DIRECTORY_DOWNLOADS}/$sub")
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
            val uri = app.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values) ?: return null
            synchronized(pending) { pending[name] = uri }
            app.contentResolver.openOutputStream(uri)
        } else {
            val dir = File(app.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS), sub).apply { mkdirs() }
            var target = File(dir, file)
            var i = 1
            while (target.exists()) target = File(dir, "${file.substringBeforeLast('.')} (${i++})" +
                (if (file.contains('.')) "." + file.substringAfterLast('.') else ""))
            synchronized(pending) { pending[name] = target }
            target.outputStream()
        }
    }

    override fun finished(name: String, ok: Boolean, message: String) {
        val item = synchronized(pending) { pending.remove(name) } ?: return
        when (item) {
            is Uri -> if (ok) {
                app.contentResolver.update(item, ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }, null, null)
            } else {
                app.contentResolver.delete(item, null, null)
            }
            is File -> if (!ok) item.delete()
        }
    }

    companion object {
        fun folderLabel() = if (Build.VERSION.SDK_INT >= 29) "Descargas/LanCast" else "Android/data/com.lancast.android/files/Download/LanCast"
    }
}
