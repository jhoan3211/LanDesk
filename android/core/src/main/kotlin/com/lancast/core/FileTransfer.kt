package com.lancast.core

import java.io.DataInputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.security.MessageDigest
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicInteger

// Canal de archivos (port de lancast-platform/files.cpp): TCP aparte (puerto de control + 1), cifrado con las
// claves "file" de la sesión. Cada archivo: FileOffer -> FileAccept/FileReject -> FileChunk... -> FileDone (SHA-256)
// -> FileResult. Funciona en los dos sentidos.

// Recepción: la app decide dónde se guarda cada archivo.
interface FileSink {
    // null = rechazar (p. ej. sin espacio). El flujo se cierra al terminar.
    fun open(name: String, size: Long): OutputStream?
    // ok = false: la app debe borrar lo que se escribió.
    fun finished(name: String, ok: Boolean, message: String)
}

data class FileProgress(
    val id: Int,
    val name: String,
    val outgoing: Boolean,
    val size: Long,
    val done: Long,
    val finished: Boolean = false,
    val ok: Boolean = false,
    val message: String = "",
)

class FileTransfer(
    private val sessionId: ByteArray,
    private val txKey: ByteArray,  // file c2h
    private val rxKey: ByteArray,  // file h2c
    private val sink: FileSink,
    private val onProgress: (FileProgress) -> Unit,
) {
    private class Outgoing(val id: Int, val name: String, val size: Long, val mtime: Long, val open: () -> InputStream)

    private class Incoming(val name: String, val size: Long, val out: OutputStream) {
        val hash: MessageDigest = MessageDigest.getInstance("SHA-256")
        var done = 0L
    }

    // Respuesta del otro lado a un envío nuestro.
    private class Reply(val kind: Int, val offset: Long = 0, val ok: Boolean = false, val message: String = "")

    @Volatile var connected = false
        private set
    private var socket: Socket? = null
    private var output: OutputStream? = null
    private val writeLock = Any()
    private var txCounter = 0L
    private val txCipher = AesGcm(txKey)  // Solo con writeLock.
    private val queue = LinkedBlockingQueue<Outgoing>()
    private val replies = HashMap<Int, Reply>()
    private val nextId = AtomicInteger(1)
    private val incoming = HashMap<Int, Incoming>()
    @Volatile private var closing = false
    private var reader: Thread? = null
    private var sender: Thread? = null

    // Bloquea (llamar fuera del hilo de la interfaz).
    fun connect(host: InetAddress, port: Int): Boolean {
        return try {
            val s = Socket()
            s.tcpNoDelay = true
            s.connect(InetSocketAddress(host, port), 4000)
            s.sendBufferSize = 1 shl 20
            socket = s
            output = s.getOutputStream()
            // FileHello en claro: el host lo asocia a la sesión activa.
            Session.writeFrame(output!!, encode(MsgType.FILE_HELLO) { it.raw(sessionId) })
            connected = true
            val input = DataInputStream(s.getInputStream().buffered(256 * 1024))
            reader = Thread({ readerLoop(input) }, "lancast-files-rx").apply {
                isDaemon = true
                start()
            }
            sender = Thread({ senderLoop() }, "lancast-files-tx").apply {
                isDaemon = true
                start()
            }
            true
        } catch (_: Exception) {
            close()
            false
        }
    }

    fun send(name: String, size: Long, mtime: Long, open: () -> InputStream): Int {
        val id = nextId.getAndIncrement()
        queue.put(Outgoing(id, name, size, mtime, open))
        onProgress(FileProgress(id, name, true, size, 0, message = "En cola"))
        return id
    }

    fun close() {
        closing = true
        connected = false
        try {
            socket?.close()
        } catch (_: Exception) {
        }
        sender?.interrupt()
        synchronized(replies) { (replies as Object).notifyAll() }
        synchronized(incoming) {
            for ((_, inc) in incoming) {
                try {
                    inc.out.close()
                } catch (_: Exception) {
                }
                sink.finished(inc.name, false, "Conexión cerrada")
            }
            incoming.clear()
        }
    }

    private fun write(msg: ByteArray): Boolean = synchronized(writeLock) {
        val out = output ?: return false
        try {
            Session.writeFrame(out, txCipher.encrypt(Session.counterNonce(txCounter), null, msg))
            txCounter++
            true
        } catch (_: Exception) {
            false
        }
    }

    // ------------------------------------------------------------------------------------------- envío

    private fun senderLoop() {
        try {
            while (!closing) sendOne(queue.take())
        } catch (_: InterruptedException) {
        }
    }

    private fun waitReply(id: Int, timeoutMs: Long): Reply? {
        val deadline = System.currentTimeMillis() + timeoutMs
        synchronized(replies) {
            while (!closing) {
                replies.remove(id)?.let { return it }
                val left = deadline - System.currentTimeMillis()
                if (left <= 0) return null
                (replies as Object).wait(left)
            }
        }
        return null
    }

    private fun sendOne(o: Outgoing) {
        fun report(done: Long, finished: Boolean = false, ok: Boolean = false, message: String = "") =
            onProgress(FileProgress(o.id, o.name, true, o.size, done, finished, ok, message))

        if (!write(encode(MsgType.FILE_OFFER) { it.u32(o.id).str(o.name).u64(o.size).u64(o.mtime) })) {
            report(0, true, false, "Conexión cerrada")
            return
        }
        report(0, message = "Esperando al PC")
        val accept = waitReply(o.id, 120_000)
        if (accept == null || accept.kind != MsgType.FILE_ACCEPT) {
            report(0, true, false, accept?.message?.ifEmpty { null } ?: "El PC no respondió")
            return
        }
        val hash = MessageDigest.getInstance("SHA-256")
        var offset = accept.offset.coerceIn(0, o.size)
        try {
            o.open().use { input ->
                // Reanudación: el hash cubre el archivo completo, así que se lee la parte que ya tiene el PC.
                val buf = ByteArray(CHUNK)
                var skip = offset
                while (skip > 0) {
                    val n = input.read(buf, 0, minOf(buf.size.toLong(), skip).toInt())
                    if (n <= 0) break
                    hash.update(buf, 0, n)
                    skip -= n
                }
                var lastReport = 0L
                while (offset < o.size && !closing) {
                    val n = input.read(buf, 0, minOf(buf.size.toLong(), o.size - offset).toInt())
                    if (n <= 0) break
                    hash.update(buf, 0, n)
                    val msg = ByteWriter(13 + n).u8(MsgType.FILE_CHUNK).u32(o.id).u64(offset).raw(buf, 0, n).toByteArray()
                    if (!write(msg)) {
                        report(offset, true, false, "Conexión cerrada")
                        return
                    }
                    offset += n
                    synchronized(replies) {
                        replies[o.id]?.takeIf { it.kind == MsgType.FILE_CANCEL }?.let {
                            report(offset, true, false, it.message.ifEmpty { "Cancelado por el PC" })
                            return
                        }
                    }
                    val now = System.currentTimeMillis()
                    if (now - lastReport > 200) {
                        report(offset)
                        lastReport = now
                    }
                }
            }
        } catch (e: Exception) {
            write(encode(MsgType.FILE_CANCEL) { it.u32(o.id).bool(false).str("Error de lectura") })
            report(offset, true, false, "No se pudo leer: ${e.message ?: e.javaClass.simpleName}")
            return
        }
        if (offset < o.size) {
            write(encode(MsgType.FILE_CANCEL) { it.u32(o.id).bool(false).str("Error de lectura") })
            report(offset, true, false, "El archivo cambió durante el envío")
            return
        }
        report(offset, message = "Verificando")
        write(encode(MsgType.FILE_DONE) { it.u32(o.id).raw(hash.digest()) })
        val result = waitReply(o.id, 600_000)
        report(offset, true, result?.kind == MsgType.FILE_RESULT && result.ok, result?.message ?: "Sin confirmación del PC")
    }

    // ------------------------------------------------------------------------------------------- recepción

    private fun readerLoop(input: DataInputStream) {
        val rx = AesGcm(rxKey)
        var counter = 0L
        try {
            while (!closing) {
                val sealed = Session.readFrame(input, MAX_FRAME) ?: break
                val msg = rx.decrypt(Session.counterNonce(counter++), null, sealed) ?: break
                if (msg.isEmpty()) continue
                handle(msg)
            }
        } catch (_: Exception) {
        }
        val wasClosing = closing
        close()
        if (!wasClosing) synchronized(replies) { (replies as Object).notifyAll() }
    }

    private fun handle(msg: ByteArray) {
        val type = msg[0].toInt() and 0xFF
        val r = ByteReader(msg, 1)
        when (type) {
            MsgType.FILE_OFFER -> {
                val id = r.u32().toInt()
                val name = r.str(4096)
                val size = r.u64()
                r.u64()  // mtime
                if (!r.ok) return
                val out = try {
                    sink.open(name, size)
                } catch (_: Exception) {
                    null
                }
                if (out == null) {
                    write(encode(MsgType.FILE_REJECT) { it.u32(id).bool(false).str("El teléfono no pudo guardar el archivo") })
                    return
                }
                synchronized(incoming) { incoming[id] = Incoming(name, size, out) }
                write(encode(MsgType.FILE_ACCEPT) { it.u32(id).u64(0) })
                onProgress(FileProgress(-id, name, false, size, 0))
            }
            MsgType.FILE_CHUNK -> {
                val id = r.u32().toInt()
                val offset = r.u64()
                if (!r.ok) return
                val inc = synchronized(incoming) { incoming[id] } ?: return
                val n = msg.size - r.position
                try {
                    if (offset != inc.done) throw IllegalStateException("Bloques fuera de orden")
                    inc.out.write(msg, r.position, n)
                    inc.hash.update(msg, r.position, n)
                    inc.done += n
                } catch (e: Exception) {
                    synchronized(incoming) { incoming.remove(id) }
                    try {
                        inc.out.close()
                    } catch (_: Exception) {
                    }
                    write(encode(MsgType.FILE_CANCEL) { it.u32(id).bool(false).str("Error de escritura en el teléfono") })
                    sink.finished(inc.name, false, e.message ?: "Error de escritura")
                    onProgress(FileProgress(-id, inc.name, false, inc.size, inc.done, true, false, "Error de escritura"))
                    return
                }
                onProgress(FileProgress(-id, inc.name, false, inc.size, inc.done))
            }
            MsgType.FILE_DONE -> {
                val id = r.u32().toInt()
                val digest = r.raw(32)
                if (!r.ok) return
                val inc = synchronized(incoming) { incoming.remove(id) } ?: return
                var ok = Crypto.equalsConstantTime(inc.hash.digest(), digest) && inc.done == inc.size
                try {
                    inc.out.close()
                } catch (_: Exception) {
                    ok = false
                }
                val message = if (ok) "Verificado (SHA-256)" else "La verificación SHA-256 falló: archivo descartado"
                write(encode(MsgType.FILE_RESULT) { it.u32(id).bool(ok).str(message) })
                sink.finished(inc.name, ok, message)
                onProgress(FileProgress(-id, inc.name, false, inc.size, inc.done, true, ok, message))
            }
            MsgType.FILE_ACCEPT, MsgType.FILE_REJECT, MsgType.FILE_RESULT, MsgType.FILE_CANCEL -> {
                val id = r.u32().toInt()
                val reply = if (type == MsgType.FILE_ACCEPT) {
                    Reply(type, offset = r.u64())
                } else {
                    val ok = r.bool()
                    Reply(type, ok = ok, message = r.str(4096))
                }
                if (!r.ok) return
                if (type == MsgType.FILE_CANCEL) {
                    // Puede ser una recepción que el PC canceló.
                    synchronized(incoming) { incoming.remove(id) }?.let { inc ->
                        try {
                            inc.out.close()
                        } catch (_: Exception) {
                        }
                        sink.finished(inc.name, false, reply.message)
                        onProgress(FileProgress(-id, inc.name, false, inc.size, inc.done, true, false, reply.message))
                    }
                }
                synchronized(replies) {
                    replies[id] = reply
                    (replies as Object).notifyAll()
                }
            }
        }
    }

    companion object {
        const val CHUNK = 256 * 1024
        private const val MAX_FRAME = (4 shl 20) + 2048  // El PC manda bloques de hasta 4 MB.
    }
}
