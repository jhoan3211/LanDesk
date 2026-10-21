package com.lancast.core

import java.io.DataInputStream
import java.io.EOFException
import java.io.OutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

// Cliente de LanCast (port de lancast-client/client_session.cpp, sin la segunda ventana).

// Emparejamientos guardados: secreto de 32 bytes por id del host (hex).
interface PairingStore {
    fun secret(hostId: String): ByteArray?
    fun save(hostId: String, name: String, address: String, secret: ByteArray)
    fun forget(hostId: String)
    // Id del equipo emparejado que estaba en esa IP (para encontrarlo si el router le dio otra).
    fun hostIdAt(address: String): String? = null
    fun updateAddress(hostId: String, address: String) {}
}

class MemoryPairingStore : PairingStore {
    private val map = HashMap<String, ByteArray>()
    private val addresses = HashMap<String, String>()
    override fun secret(hostId: String) = synchronized(map) { map[hostId] }
    override fun save(hostId: String, name: String, address: String, secret: ByteArray) {
        synchronized(map) {
            map[hostId] = secret
            addresses[hostId] = address
        }
    }

    override fun forget(hostId: String) {
        synchronized(map) { map.remove(hostId) }
    }

    override fun hostIdAt(address: String) = synchronized(map) { addresses.entries.firstOrNull { it.value == address }?.key }
    override fun updateAddress(hostId: String, address: String) {
        synchronized(map) { addresses[hostId] = address }
    }
}

// Eventos de la sesión (llegan desde hilos de red).
interface SessionListener {
    // El host pide PIN: llamar a Session.submitPin (o close para cancelar).
    fun onNeedPin(hostName: String) {}
    fun onConnected(hostName: String) {}
    fun onHostInfo(info: HostInfo) {}
    fun onStreamStarted(stream: StreamStarted) {}
    fun onVideoFrame(frame: AssembledFrame, arrivalUs: Long) {}
    fun onAudio(pcm: ShortArray, sampleRate: Int, channels: Int) {}
    fun onCursorPos(pos: CursorPos) {}
    fun onCursorShape(shape: CursorShape) {}
    fun onClipboard(text: String) {}
    fun onRumble(rumble: GamepadRumble) {}
    fun onMessage(text: String) {}
    fun onClosed(reason: String, retryable: Boolean) {}
}

// Una ventana de 250 ms, para los gráficos en tiempo real.
data class StatSample(
    val timeUs: Long,
    val fps: Float,
    val mbps: Float,
    val networkMs: Float,     // Promedio de la ventana.
    val networkMaxMs: Float,  // Peor cuadro de la ventana (los picos).
    val lost: Int,            // Cuadros perdidos en la ventana.
    val recovered: Int,       // Cuadros reparados con FEC.
    val rttMs: Float,
)

data class SessionStats(
    val rttMs: Double = 0.0,
    val fps: Double = 0.0,
    val bitrateMbps: Double = 0.0,
    val networkMs: Double = 0.0,
    val framesRecovered: Long = 0,
    val framesLost: Long = 0,
    val keyframeRequests: Long = 0,
)

class Session(
    private val deviceId: ByteArray,
    private val deviceName: String,
    private val pairing: PairingStore,
    private val listener: SessionListener,
    // Redes donde buscar a un equipo emparejado que cambió de IP (la app pasa las del Wi-Fi o el cable).
    private val broadcast: () -> List<InetAddress> = { listOf(InetAddress.getByName("255.255.255.255")) },
) {
    @Volatile var hostName = ""
        private set
    @Volatile var hostInfo: HostInfo? = null
        private set
    @Volatile var stream: StreamStarted? = null
        private set
    @Volatile var stats = SessionStats()
        private set

    private val stopping = AtomicBoolean(false)
    private val pin = AtomicReference<String?>(null)
    private var pinLatch = CountDownLatch(1)
    private var socket: Socket? = null
    private var output: OutputStream? = null
    // Envío: todo pasa por una cola y un hilo propio. Así nunca se usa la red desde el hilo de la interfaz
    // (Android lo prohíbe) y el contador del nonce solo avanza cuando la trama salió de verdad: si avanzara con
    // un envío fallido, el host rechazaría el siguiente mensaje y cortaría la sesión.
    private val outbox = java.util.concurrent.LinkedBlockingQueue<ByteArray>()
    @Volatile private var senderThread: Thread? = null
    private var mediaSocket: DatagramSocket? = null
    private var controlThread: Thread? = null
    private var mediaThread: Thread? = null
    private var mediaRxKey = ByteArray(0)
    private var mediaTxKey = ByteArray(0)
    private var fileTxKey = ByteArray(0)
    private var fileRxKey = ByteArray(0)
    private var sessionId = ByteArray(16)
    private val history = ArrayDeque<StatSample>()
    @Volatile var hostAddress: InetAddress? = null
        private set

    // Medios (solo el hilo de medios).
    private val assembler = FrameAssembler()
    private val joiner = SliceJoiner()
    @Volatile private var waitingKeyframe = true
    @Volatile private var lastFrameUs = 0L
    @Volatile private var lastIdrRequestUs = 0L
    private var keyframeRequests = 0L
    private var clockOffsetUs = 0L  // reloj del host - reloj local (por ping)
    private var hasClock = false

    // FEC automático: la paridad se ajusta según las pérdidas (sin pérdidas baja hasta 5 %, con pérdidas sube hasta
    // 40 %). Cambiarla no reinicia el codificador del host ni corta la imagen.
    @Volatile var autoFec = true
    @Volatile var currentFec = 10
        private set
    @Volatile private var lastConfig: StreamConfig? = null
    private var fecCleanSinceUs = 0L
    private var fecTroubles = 0  // Muestras con pérdidas desde fecCleanSinceUs.

    private fun withFec(config: StreamConfig): StreamConfig {
        val c = if (autoFec) config.copy(fecPercent = currentFec) else config
        lastConfig = c
        return c
    }

    fun connect(host: String, port: Int, caps: ClientCaps, config: StreamConfig) {
        val first = withFec(config)
        controlThread = Thread({ controlLoop(host, port, caps, first) }, "lancast-control").apply { start() }
    }

    // Una ventana de 250 ms: pérdida de paquetes (%), cuadros perdidos y cuadros reparados con la paridad.
    // Envío cifrado por UDP al host (desde el hilo de medios y desde el del micrófono).
    private val txLock = Any()
    private var mediaTx: AesGcm? = null
    private var mediaTarget: Pair<java.net.InetAddress, Int>? = null
    private var txSeq = 1L
    private var micSeq = 0L

    private fun sendMedia(type: Int, body: ByteArray) {
        val datagram = synchronized(txLock) {
            val tx = mediaTx ?: return
            val header = packetHeader(type, txSeq++)
            header + tx.encrypt(header, header, body)
        }
        val (host, port) = mediaTarget ?: return
        try {
            mediaSocket?.send(DatagramPacket(datagram, datagram.size, host, port))
        } catch (_: Exception) {
        }
    }

    // Micrófono de este teléfono hacia el PC (PCM de 16 bits mono). Solo si el host lo acepta (hostInfo.micAvailable).
    fun sendMic(pcm: ShortArray, frames: Int, sampleRate: Int) {
        if (hostInfo?.micAvailable != true) return
        val chunk = maxOf(sampleRate / 100, 1)
        var off = 0
        while (off < frames) {
            val n = minOf(chunk, frames - off)
            val w = ByteWriter().u32(++micSeq and 0xFFFFFFFFL).u64(nowUs()).u32(sampleRate.toLong()).u8(1).u16(n)
            for (i in off until off + n) w.u16(pcm[i].toInt() and 0xFFFF)
            sendMedia(MediaConst.TYPE_MIC, w.toByteArray())
            off += n
        }
    }

    private fun autoFecTick(lossPct: Double, lost: Int, recovered: Int, now: Long) {
        val cfg = lastConfig ?: return
        if (!autoFec) return
        val cur = currentFec
        var next = cur
        if (fecCleanSinceUs == 0L) fecCleanSinceUs = now
        if (lossPct > 0.3 || lost > 0 || recovered > 0) {
            fecTroubles++
            val target = (Math.ceil(lossPct * 3).toInt() + 10).coerceIn(10, 40)
            next = minOf(40, maxOf(target, cur + if (lost > 0) 10 else 0))
            if (next > cur) {
                fecCleanSinceUs = now
                fecTroubles = 0
            }
        } else if (now - fecCleanSinceUs > 15_000_000) {
            // 15 s sin pérdidas (o con un solo tropiezo aislado): baja de a 5 %.
            if (fecTroubles <= 1 && cur > 5) next = maxOf(5, cur - 5)
            fecCleanSinceUs = now
            fecTroubles = 0
        }
        if (next == cur) return
        currentFec = next
        val c = cfg.copy(fecPercent = next)
        lastConfig = c
        send(c.encode())
    }

    fun submitPin(value: String) {
        pin.set(value)
        pinLatch.countDown()
    }

    // No bloquea: se puede llamar desde cualquier hilo (también el de la interfaz).
    fun send(message: ByteArray): Boolean {
        if (stopping.get() || senderThread == null) return false
        return outbox.offer(message)
    }

    private fun senderLoop(cipher: AesGcm, out: OutputStream) {
        var counter = 0L
        try {
            while (true) {
                val msg = outbox.take()
                if (msg === CLOSE_MARKER) break
                writeFrame(out, cipher.encrypt(counterNonce(counter), null, msg))
                counter++  // Solo si se escribió.
            }
        } catch (_: InterruptedException) {
        } catch (_: Exception) {
            // La escritura falló: el canal ya no sirve. Al cerrar el socket, el hilo de control avisa y se reconecta.
        } finally {
            closeSockets()
        }
    }

    private fun closeSockets() {
        try {
            socket?.close()
        } catch (_: Exception) {
        }
        mediaSocket?.close()
    }

    fun requestStream(config: StreamConfig) = send(withFec(config).encode())

    // Últimos 30 s en ventanas de 250 ms (copia).
    fun history(): List<StatSample> = synchronized(history) { history.toList() }

    // Canal de archivos con el PC (bloquea: llamar fuera del hilo de la interfaz). null si el PC no lo permite.
    fun openFiles(sink: FileSink, onProgress: (FileProgress) -> Unit): FileTransfer? {
        val info = hostInfo ?: return null
        val host = hostAddress ?: return null
        if (!info.filesAllowed || info.filePort == 0 || stopping.get()) return null
        val ft = FileTransfer(sessionId, fileTxKey, fileRxKey, sink, onProgress)
        return if (ft.connect(host, info.filePort)) ft else null
    }

    fun requestKeyframe(reason: Int = 0) {
        val now = nowUs()
        if (now - lastIdrRequestUs < (if (reason == 1) 300_000 else 30_000)) return
        lastIdrRequestUs = now
        keyframeRequests++
        send(Messages.requestKeyframe(reason))
    }

    fun close() {
        if (stopping.getAndSet(true)) return
        pinLatch.countDown()
        if (senderThread != null) {
            // Soltar teclas/botones y avisar al host; el hilo de envío cierra el socket al terminar.
            outbox.offer(Messages.releaseAll())
            outbox.offer(Messages.disconnect())
            outbox.offer(CLOSE_MARKER)
            Thread {
                Thread.sleep(500)  // Por si la red está trabada.
                closeSockets()
            }.apply { isDaemon = true }.start()
        } else {
            Thread { closeSockets() }.apply { isDaemon = true }.start()
        }
    }

    // ------------------------------------------------------------------------------------------- control

    private fun controlLoop(host: String, port: Int, caps: ClientCaps, config: StreamConfig) {
        var reason = "Desconectado"
        var retryable = true
        try {
            // A un equipo emparejado se le da poco tiempo en su IP de siempre; si no responde, el router (DHCP) pudo
            // haberle dado otra: se lo busca en la red por su id, que no cambia, y se entra sin PIN.
            val pairedId = pairing.hostIdAt(host)
            var sock = Socket()
            sock.tcpNoDelay = true
            try {
                sock.connect(InetSocketAddress(host, port), if (pairedId != null) 1500 else 4000)
            } catch (e: Exception) {
                if (pairedId == null || stopping.get()) throw e
                val moved = (0 until 2).firstNotNullOfOrNull {
                    runCatching { discover(1200, broadcast()) }.getOrDefault(emptyList()).firstOrNull { h -> h.deviceId == pairedId }
                } ?: throw e
                sock = Socket()
                sock.tcpNoDelay = true
                sock.connect(InetSocketAddress(moved.address, moved.controlPort), 4000)
            }
            socket = sock
            hostAddress = sock.inetAddress
            output = sock.getOutputStream()
            val input = DataInputStream(sock.getInputStream().buffered(64 * 1024))
            sock.soTimeout = 10_000
            val recvCipher = handshake(sock, input) ?: return
            sock.soTimeout = 15_000
            listener.onConnected(hostName)
            send(caps.encode())
            send(config.encode())

            var recvCounter = 0L
            while (!stopping.get()) {
                val sealed = readFrame(input, 8 shl 20) ?: break
                val msg = recvCipher.decrypt(counterNonce(recvCounter++), null, sealed)
                if (msg == null) {
                    reason = "Canal cifrado inválido"
                    retryable = false
                    break
                }
                if (msg.isEmpty()) continue
                handleMessage(msg)
            }
        } catch (e: SocketTimeoutException) {
            reason = "El host dejó de responder"
        } catch (e: EOFException) {
            reason = "El host cerró la conexión"
        } catch (e: Exception) {
            if (!stopping.get()) reason = "No se pudo conectar: ${e.message ?: e.javaClass.simpleName}"
        } finally {
            val wasStopping = stopping.getAndSet(true)
            outbox.offer(CLOSE_MARKER)  // Termina el hilo de envío.
            closeSockets()
            if (!wasStopping) listener.onClosed(reason, retryable) else listener.onClosed("Desconectado", false)
        }
    }

    // Devuelve el descifrador del canal de control, o null si falló (ya notificado).
    private fun handshake(sock: Socket, input: DataInputStream): AesGcm? {
        val out = output!!
        val ecdh = Ecdh()
        val helloRaw = Hello(deviceId, deviceName, ecdh.publicKey, Crypto.randomBytes(32)).encode()
        writeFrame(out, helloRaw)
        val ackRaw = readFrame(input, 64 * 1024) ?: return fail("El host no respondió", true)
        val ack = parse(ackRaw, MsgType.HELLO_ACK)?.let { HelloAck.read(it) } ?: return fail("Respuesta inválida del host", false)
        hostName = ack.deviceName
        if (!ack.accepted) return fail(ack.message, ack.message.contains("sesión activa"))
        if (ack.version != Proto.VERSION) return fail("Versión de LanCast distinta en el PC (actualízalo)", false)

        val th = SessionCrypto.transcript(helloRaw, ackRaw)
        val z = ecdh.derive(ack.publicKey) ?: return fail("Clave del host inválida", false)
        val hostId = ack.deviceId.toHex()
        val known = pairing.secret(hostId)
        val usePairing = ack.hasPairing && known != null
        writeFrame(out, encode(MsgType.AUTH_START) { it.bool(usePairing) })

        val startRaw = readFrame(input, 64 * 1024) ?: return fail("El host cerró la conexión", true)
        val pinRequired = parse(startRaw, MsgType.AUTH_START_ACK)?.bool() ?: return fail("Respuesta inválida del host", false)

        val authKey = if (pinRequired) {
            pinLatch = CountDownLatch(1)
            pin.set(null)
            sock.soTimeout = 0
            listener.onNeedPin(hostName)
            if (!pinLatch.await(115, TimeUnit.SECONDS) || stopping.get()) {
                return if (stopping.get()) null else fail("Tiempo de espera del PIN agotado", false)
            }
            SessionCrypto.authKeyFromPin(pin.get() ?: "", th)
        } else {
            SessionCrypto.authKeyFromSecret(known!!, th)
        }

        writeFrame(out, encode(MsgType.AUTH_PROOF) { it.raw(SessionCrypto.proof(authKey, "client", th, z)) })
        sock.soTimeout = 15_000
        val resultRaw = readFrame(input, 64 * 1024) ?: return fail("El host cerró la conexión durante la autenticación", true)
        val result = parse(resultRaw, MsgType.AUTH_RESULT)?.let { AuthResult.read(it) }
        if (result == null || !result.ok) {
            if (usePairing) pairing.forget(hostId)
            return fail(result?.message ?: "Autenticación rechazada", false)
        }
        if (!Crypto.equalsConstantTime(result.mac, SessionCrypto.proof(authKey, "host", th, z))) {
            return fail("El host no pudo demostrar su identidad", false)
        }
        val keys = SessionCrypto.deriveKeys(z, authKey, th)
        if (pinRequired) pairing.save(hostId, ack.deviceName, sock.inetAddress.hostAddress ?: "", keys.pairingSecret)
        else pairing.updateAddress(hostId, sock.inetAddress.hostAddress ?: "")
        mediaRxKey = keys.mediaH2c
        mediaTxKey = keys.mediaC2h
        fileTxKey = keys.fileC2h
        fileRxKey = keys.fileH2c
        sessionId = result.sessionId
        val cipher = AesGcm(keys.controlC2h)
        senderThread = Thread({ senderLoop(cipher, output!!) }, "lancast-send").apply {
            isDaemon = true
            start()
        }
        return AesGcm(keys.controlH2c)
    }

    private fun fail(message: String, retryable: Boolean): AesGcm? {
        if (!stopping.getAndSet(true)) listener.onClosed(message, retryable)
        return null
    }

    private fun handleMessage(msg: ByteArray) {
        val r = ByteReader(msg, 1)
        when (msg[0].toInt() and 0xFF) {
            MsgType.HOST_INFO -> HostInfo.read(r)?.let { info ->
                val first = hostInfo == null
                hostInfo = info
                listener.onHostInfo(info)
                if (first) mediaThread = Thread({ mediaLoop(info.mediaPort) }, "lancast-media").apply {
                    priority = Thread.MAX_PRIORITY
                    start()
                }
            }
            MsgType.STREAM_STARTED -> StreamStarted.read(r)?.let { s ->
                if (s.streamIndex != 0) return
                // Mismo stream (cambió solo el bitrate, el FEC o el audio): la imagen sigue sin esperar un keyframe.
                val old = stream
                val same = old != null && old.epoch == s.epoch && old.codec == s.codec && old.width == s.width && old.height == s.height
                stream = s
                if (!same) waitingKeyframe = true
                listener.onStreamStarted(s)
            }
            MsgType.STREAM_ERROR -> listener.onMessage(r.str(4096))
            MsgType.PONG -> {
                val sent = r.u64()
                val hostTime = r.u64()
                if (r.ok) {
                    val now = nowUs()
                    val rtt = now - sent
                    // Reloj del host respecto del local (estimado en la mitad del viaje).
                    clockOffsetUs = hostTime - (sent + rtt / 2)
                    hasClock = true
                    stats = stats.copy(rttMs = rtt / 1000.0)
                }
            }
            MsgType.CURSOR_POS -> CursorPos.read(r)?.let { if (it.streamIndex == 0) listener.onCursorPos(it) }
            MsgType.CURSOR_SHAPE -> CursorShape.read(r)?.let { listener.onCursorShape(it) }
            MsgType.CLIPBOARD -> listener.onClipboard(r.str())
            MsgType.GAMEPAD_RUMBLE -> GamepadRumble.read(r)?.let { listener.onRumble(it) }
            MsgType.DISCONNECT -> {
                if (!stopping.getAndSet(true)) listener.onClosed("El host cerró la sesión", false)
                try {
                    socket?.close()
                } catch (_: Exception) {
                }
            }
        }
    }

    // ------------------------------------------------------------------------------------------- medios

    private fun mediaLoop(mediaPort: Int) {
        val sock = DatagramSocket()
        mediaSocket = sock
        try {
            sock.receiveBufferSize = 8 shl 20
        } catch (_: Exception) {
        }
        sock.soTimeout = 20
        val host = hostAddress ?: return
        val rx = AesGcm(mediaRxKey)
        synchronized(txLock) {
            mediaTx = AesGcm(mediaTxKey)
            mediaTarget = host to mediaPort
        }
        val buf = ByteArray(2048)
        val packet = DatagramPacket(buf, buf.size)
        var lastHello = 0L
        var lastPing = 0L
        var windowStart = nowUs()
        var windowFrames = 0
        var windowBytes = 0L
        var windowNetUs = 0L
        // Ventanas cortas para los gráficos.
        var gStart = nowUs()
        var gFrames = 0
        var gBytes = 0L
        var gNetUs = 0L
        var gNetMaxUs = 0L
        var gLost = assembler.framesLost
        var gRecovered = assembler.framesRecovered
        var gPackets = 0L
        var gMinSeq = Long.MAX_VALUE
        var gMaxSeq = Long.MIN_VALUE

        while (!stopping.get()) {
            val now = nowUs()
            if (now - lastHello > 500_000) {
                // Registra/renueva el endpoint UDP en el host (tráfico saliente: atraviesa el firewall del celular).
                sendMedia(MediaConst.TYPE_CLIENT_HELLO, byteArrayOf(1))
                lastHello = now
            }
            if (now - lastPing > 1_000_000) {
                send(Messages.ping(nowUs()))
                lastPing = now
            }
            if (now - windowStart >= 1_000_000) {
                val secs = (now - windowStart) / 1e6
                stats = stats.copy(
                    fps = windowFrames / secs,
                    bitrateMbps = windowBytes * 8 / secs / 1e6,
                    networkMs = if (windowFrames > 0) windowNetUs / 1000.0 / windowFrames else stats.networkMs,
                    framesRecovered = assembler.framesRecovered,
                    framesLost = assembler.framesLost,
                    keyframeRequests = keyframeRequests,
                )
                windowStart = now
                windowFrames = 0
                windowBytes = 0
                windowNetUs = 0
            }
            if (now - gStart >= 250_000) {
                val secs = (now - gStart) / 1e6
                val sample = StatSample(
                    now, (gFrames / secs).toFloat(), (gBytes * 8 / secs / 1e6).toFloat(),
                    if (gFrames > 0) gNetUs / 1000f / gFrames else 0f, gNetMaxUs / 1000f,
                    (assembler.framesLost - gLost).toInt(), (assembler.framesRecovered - gRecovered).toInt(), stats.rttMs.toFloat(),
                )
                synchronized(history) {
                    history.addLast(sample)
                    while (history.size > 120) history.removeFirst()
                }
                gStart = now
                gFrames = 0
                gBytes = 0
                gNetUs = 0
                gNetMaxUs = 0
                val expected = if (gPackets > 0 && gMaxSeq >= gMinSeq) (gMaxSeq - gMinSeq + 1).toDouble() else 0.0
                val lossPct = if (expected > 0) ((expected - gPackets) * 100.0 / expected).coerceAtLeast(0.0) else 0.0
                autoFecTick(lossPct, sample.lost, sample.recovered, now)
                gLost = assembler.framesLost
                gRecovered = assembler.framesRecovered
                gPackets = 0
                gMinSeq = Long.MAX_VALUE
                gMaxSeq = Long.MIN_VALUE
            }

            try {
                packet.setData(buf, 0, buf.size)
                sock.receive(packet)
            } catch (_: SocketTimeoutException) {
                watchdog()
                continue
            } catch (_: Exception) {
                break
            }
            val n = packet.length
            if (n < MediaConst.PACKET_HEADER_SIZE + MediaConst.TAG_SIZE || packet.address != host) continue
            windowBytes += n
            gBytes += n
            val header = buf.copyOf(MediaConst.PACKET_HEADER_SIZE)
            val payload = rx.decrypt(header, header, buf, MediaConst.PACKET_HEADER_SIZE, n - MediaConst.PACKET_HEADER_SIZE) ?: continue
            when (header[0].toInt()) {
                MediaConst.TYPE_VIDEO -> {
                    // Secuencia del datagrama (para medir la pérdida de paquetes de video).
                    var seq = 0L
                    for (i in 0 until 8) seq = seq or ((header[4 + i].toLong() and 0xFF) shl (8 * i))
                    gPackets++
                    if (seq < gMinSeq) gMinSeq = seq
                    if (seq > gMaxSeq) gMaxSeq = seq
                    val frame = handleVideo(payload)
                    if (frame != null) {
                        val arrival = nowUs()
                        windowFrames++
                        gFrames++
                        if (hasClock) {
                            val net = (arrival + clockOffsetUs - frame.info.captureTimeUs - frame.info.encodeTimeUs).coerceIn(0, 1_000_000)
                            windowNetUs += net
                            gNetUs += net
                            if (net > gNetMaxUs) gNetMaxUs = net
                        }
                        listener.onVideoFrame(frame, arrival)
                    }
                    watchdog()
                }
                MediaConst.TYPE_AUDIO -> handleAudio(payload)
            }
        }
    }

    private fun handleVideo(payload: ByteArray): AssembledFrame? {
        val r = ByteReader(payload)
        val h = VideoShardHeader.read(r) ?: return null
        if (h.flags and MediaConst.FLAG_STREAM1 != 0) return null  // Segunda ventana: no se usa en el celular.
        val unit = assembler.add(h, payload, r.position, payload.size - r.position)
        val frame = unit?.let { joiner.add(it) }
        val lost = assembler.takeLost() + joiner.takeLost()
        if (lost > 0) {
            if (stream?.intraRefresh == true && !waitingKeyframe) {
                requestKeyframe(1)
            } else {
                waitingKeyframe = true
                requestKeyframe(0)
            }
        }
        if (frame == null) return null
        lastFrameUs = nowUs()
        val s = stream
        if (s == null || frame.info.epoch != s.epoch) return null  // Cuadro de otra configuración.
        if (waitingKeyframe && !frame.info.keyframe) {
            requestKeyframe(0)
            return null
        }
        if (frame.info.keyframe) waitingKeyframe = false
        return frame
    }

    // Llegan paquetes pero ningún cuadro se completa hace >250 ms (p. ej. se perdió el keyframe): pedir otro.
    private fun watchdog() {
        val t = nowUs()
        if (lastFrameUs != 0L && stream != null) {
            val since = t - lastFrameUs
            if (since in 250_001..4_999_999 && t - lastIdrRequestUs > 250_000) requestKeyframe(0)
        }
    }

    private fun handleAudio(payload: ByteArray) {
        val r = ByteReader(payload)
        r.u32()  // secuencia
        r.u64()  // tiempo de captura
        val rate = r.u32().toInt()
        val channels = r.u8()
        val frames = r.u16()
        if (!r.ok || channels == 0) return
        val bytes = r.raw(frames * channels * 2)
        if (bytes.size != frames * channels * 2) return
        val pcm = ShortArray(frames * channels)
        java.nio.ByteBuffer.wrap(bytes).order(java.nio.ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(pcm)
        listener.onAudio(pcm, rate, channels)
    }

    companion object {
        private val CLOSE_MARKER = ByteArray(0)

        fun nowUs(): Long = System.nanoTime() / 1000

        fun counterNonce(counter: Long): ByteArray {
            val n = ByteArray(12)
            for (i in 0 until 8) n[4 + i] = (counter ushr (8 * i)).toByte()
            return n
        }

        fun writeFrame(out: OutputStream, data: ByteArray) {
            val len = data.size
            val frame = ByteArray(4 + len)
            for (i in 0 until 4) frame[i] = (len ushr (8 * i)).toByte()
            System.arraycopy(data, 0, frame, 4, len)
            out.write(frame)
            out.flush()
        }

        fun readFrame(input: DataInputStream, max: Int): ByteArray? {
            val header = ByteArray(4)
            input.readFully(header)
            val len = (header[0].toInt() and 0xFF) or ((header[1].toInt() and 0xFF) shl 8) or
                ((header[2].toInt() and 0xFF) shl 16) or ((header[3].toInt() and 0xFF) shl 24)
            if (len < 0 || len > max) return null
            val data = ByteArray(len)
            input.readFully(data)
            return data
        }

        private fun parse(raw: ByteArray, type: Int): ByteReader? =
            if (raw.isNotEmpty() && (raw[0].toInt() and 0xFF) == type) ByteReader(raw, 1) else null

        // Descubrimiento: broadcast UDP; los hosts responden con su nombre e id.
        fun discover(timeoutMs: Int, broadcast: List<InetAddress> = listOf(InetAddress.getByName("255.255.255.255"))): List<DiscoveredHost> {
            val found = LinkedHashMap<String, DiscoveredHost>()
            DatagramSocket().use { sock ->
                sock.broadcast = true
                sock.soTimeout = 100
                val req = ByteWriter().u32(Proto.DISCOVERY_MAGIC).u8(0).toByteArray()
                for (addr in broadcast) {
                    try {
                        sock.send(DatagramPacket(req, req.size, addr, Proto.DISCOVERY_PORT))
                    } catch (_: Exception) {
                    }
                }
                val buf = ByteArray(2048)
                val deadline = System.currentTimeMillis() + timeoutMs
                while (System.currentTimeMillis() < deadline) {
                    val p = DatagramPacket(buf, buf.size)
                    try {
                        sock.receive(p)
                    } catch (_: SocketTimeoutException) {
                        continue
                    }
                    val r = ByteReader(buf, 0, p.length)
                    if (r.u32().toInt() != Proto.DISCOVERY_MAGIC) continue
                    r.u16()
                    val name = r.str(256)
                    val id = r.raw(16).toHex()
                    val port = r.u16()
                    r.u16()
                    val busy = r.bool()
                    if (r.ok) found[id] = DiscoveredHost(name, id, p.address.hostAddress ?: "", port, busy)
                }
            }
            return found.values.toList()
        }
    }
}
