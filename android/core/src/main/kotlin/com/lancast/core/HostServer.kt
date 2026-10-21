package com.lancast.core

import java.io.DataInputStream
import java.io.OutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.security.SecureRandom
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.locks.LockSupport

// Host de LanCast en Kotlin (port de lancast-host/host_service.cpp): el teléfono comparte su pantalla y recibe
// el control. Mismo protocolo que el host de Windows, así que lo usan el cliente de PC y el de Android sin cambios.
// La captura, el codificador y la inyección de toques los pone la app (HostListener).

// Pedido de stream del cliente (StreamConfig del protocolo).
data class StreamRequest(
    val monitorId: Int,
    val maxWidth: Int,
    val maxHeight: Int,
    val fps: Int,
    val codec: Int,  // 0 = automático, 1 = H.264, 2 = HEVC, 3 = AV1
    val autoBitrate: Boolean,
    val bitrateKbps: Int,
    val audio: Boolean,
    val fecPercent: Int,
    val audioRate: Int,
    val audioChannels: Int,
    val streamIndex: Int,
    val enabled: Boolean,
    val pacingMbps: Int,
)

data class ClientInfo(val name: String, val decoders: List<Codec>, val displayWidth: Int, val displayHeight: Int, val displayRefresh: Int)

sealed class RemoteInput {
    data class MouseAbs(val x: Float, val y: Float) : RemoteInput()
    data class MouseButton(val button: Int, val down: Boolean) : RemoteInput()  // 0 izq, 1 der, 2 medio, 3 X1, 4 X2
    data class Wheel(val delta: Int, val horizontal: Boolean) : RemoteInput()
    data class Key(val scancode: Int, val extended: Boolean, val down: Boolean) : RemoteInput()
    data class Text(val text: String) : RemoteInput()
    data class Clipboard(val text: String) : RemoteInput()
    object ReleaseAll : RemoteInput()
}

// Clientes emparejados con este host (secreto de 32 bytes por id del cliente en hex).
interface HostPairingStore {
    fun clientSecret(clientId: String): ByteArray?
    fun saveClient(clientId: String, name: String, address: String, secret: ByteArray)
}

// Lo que el host anuncia en HostInfo (lo arma la app).
data class HostDescription(
    val name: String,
    val screenWidth: Int,
    val screenHeight: Int,
    val refreshHz: Int,
    val encoders: List<Codec>,
    val audioAvailable: Boolean,
    val inputAllowed: Boolean,
    val clipboardAllowed: Boolean,
)

interface HostListener {
    fun onPin(pin: String, requester: String) {}
    fun onPinDone() {}
    fun onSessionStarted(client: ClientInfo, address: String) {}
    fun onSessionEnded(clientName: String) {}
    // La app (re)configura captura y codificador y luego llama a HostServer.streamStarted().
    fun onStreamRequested(client: ClientInfo, request: StreamRequest) {}
    fun onKeyframeRequested() {}
    fun onInput(input: RemoteInput) {}
}

class HostServer(
    private val deviceId: ByteArray,
    private val pairing: HostPairingStore,
    private val listener: HostListener,
    private val describe: () -> HostDescription,
) {
    private val running = AtomicBoolean(false)
    private var control: ServerSocket? = null
    private var media: DatagramSocket? = null
    private var discovery: DatagramSocket? = null
    private val random = SecureRandom()
    private val sessionLock = Object()
    @Volatile private var session: HostSession? = null
    @Volatile private var handshaking = false
    private val failures = HashMap<String, Pair<Int, Long>>()  // ip -> (fallos, último)
    var controlPort = Proto.DEFAULT_CONTROL_PORT
        private set
    var mediaPort = DEFAULT_MEDIA_PORT
        private set

    val hasSession: Boolean get() = session != null
    val clientName: String? get() = session?.client?.name

    // Abre los puertos (si el de control está ocupado, prueba los siguientes). false si no se pudo.
    fun start(preferredControlPort: Int = Proto.DEFAULT_CONTROL_PORT, preferredMediaPort: Int = DEFAULT_MEDIA_PORT): Boolean {
        if (running.getAndSet(true)) return true
        try {
            control = openServer(preferredControlPort)
            controlPort = control!!.localPort
            media = openUdp(preferredMediaPort)
            mediaPort = media!!.localPort
            media!!.sendBufferSize = 4 shl 20
            try {
                discovery = DatagramSocket(null).apply {
                    reuseAddress = true
                    bind(InetSocketAddress(Proto.DISCOVERY_PORT))
                }
            } catch (_: Exception) {
                discovery = null  // Sin descubrimiento: igual se puede conectar por IP.
            }
        } catch (e: Exception) {
            stop()
            return false
        }
        thread("lancast-host-accept") { acceptLoop() }
        thread("lancast-host-media") { mediaLoop() }
        if (discovery != null) thread("lancast-host-discovery") { discoveryLoop() }
        return true
    }

    fun stop() {
        if (!running.getAndSet(false)) return
        session?.close()
        runCatching { control?.close() }
        runCatching { media?.close() }
        runCatching { discovery?.close() }
    }

    fun disconnectClient() {
        session?.close()
    }

    // --------------------------------------------------------------------------------------- envío de medios

    // El stream quedó configurado: avisa al cliente. Devuelve la época (los paquetes viejos se descartan).
    fun streamStarted(codec: Codec, width: Int, height: Int, fps: Int, bitrateKbps: Int, audioRate: Int, audioChannels: Int,
                      backend: String, notes: String = ""): Int {
        val s = session ?: return 0
        return s.streamStarted(codec, width, height, fps, bitrateKbps, audioRate, audioChannels, backend, notes)
    }

    fun sendVideo(data: ByteArray, length: Int, keyframe: Boolean, captureUs: Long, encodeUs: Long) {
        session?.sendVideo(data, length, keyframe, captureUs, encodeUs)
    }

    fun sendAudio(pcm: ShortArray, frames: Int, rate: Int, channels: Int) {
        session?.sendAudio(pcm, frames, rate, channels)
    }

    fun sendCursor(x: Float, y: Float, visible: Boolean) {
        session?.send(encode(MsgType.CURSOR_POS) { it.bool(visible).f32(x).f32(y).u8(0) })
    }

    // Cursor de color (BGRA, sin premultiplicar) para que el cliente de PC dibuje un puntero.
    fun sendCursorShape(width: Int, height: Int, hotX: Int, hotY: Int, bgra: ByteArray) {
        session?.send(encode(MsgType.CURSOR_SHAPE) {
            it.u8(2).u32(width).u32(height).u32(width * 4).i32(hotX).i32(hotY).u32(bgra.size).raw(bgra)
        })
    }

    fun sendClipboard(text: String) {
        session?.send(Messages.clipboard(text))
    }

    // --------------------------------------------------------------------------------------- conexiones

    private fun acceptLoop() {
        while (running.get()) {
            val sock = try {
                control!!.accept()
            } catch (_: Exception) {
                if (!running.get()) return
                continue
            }
            thread("lancast-host-conn") {
                try {
                    handleConnection(sock)
                } catch (_: Exception) {
                } finally {
                    runCatching { sock.close() }
                }
            }
        }
    }

    private fun handleConnection(sock: Socket) {
        sock.tcpNoDelay = true
        sock.soTimeout = 10_000
        val ip = sock.inetAddress.hostAddress ?: ""
        val input = DataInputStream(sock.getInputStream().buffered(64 * 1024))
        val out = sock.getOutputStream()
        val helloRaw = Session.readFrame(input, 64 * 1024) ?: return
        if (helloRaw.isEmpty() || (helloRaw[0].toInt() and 0xFF) != MsgType.HELLO) return
        val hr = ByteReader(helloRaw, 1)
        val magic = hr.u32().toInt()
        val version = hr.u16()
        val clientId = hr.raw(16)
        val clientName = hr.str(256)
        val clientPub = hr.raw(64)
        hr.raw(32)  // nonce
        if (!hr.ok || magic != Proto.MAGIC) return
        val desc = describe()
        val ecdh = Ecdh()
        fun ack(accepted: Boolean, message: String, hasPairing: Boolean) = encode(MsgType.HELLO_ACK) {
            it.u16(Proto.VERSION).raw(deviceId).str(desc.name).raw(ecdh.publicKey).raw(Crypto.randomBytes(32))
                .bool(hasPairing).bool(accepted).str(message)
        }
        if (version != Proto.VERSION) {
            Session.writeFrame(out, ack(false, "Versión de LanCast distinta: actualiza la app en los dos equipos", false))
            return
        }
        val cid = clientId.toHex()
        val known = pairing.clientSecret(cid)
        val reject = synchronized(sessionLock) {
            val f = failures[ip]
            when {
                f != null && f.first >= 5 && System.currentTimeMillis() - f.second < 600_000 -> "Demasiados intentos fallidos. Espera 10 minutos."
                session != null && session!!.clientId != cid -> "${desc.name} ya tiene una sesión activa"
                handshaking -> "Hay otro emparejamiento en curso"
                else -> null
            }
        }
        val ackRaw = ack(reject == null, reject ?: "", known != null)
        Session.writeFrame(out, ackRaw)
        if (reject != null) return
        synchronized(sessionLock) {
            if (handshaking) return
            handshaking = true
        }
        var pinShown = false
        try {
            val th = SessionCrypto.transcript(helloRaw, ackRaw)
            val z = ecdh.derive(clientPub) ?: return
            val startRaw = Session.readFrame(input, 1024) ?: return
            if (startRaw.size < 2 || (startRaw[0].toInt() and 0xFF) != MsgType.AUTH_START) return
            val usePairing = startRaw[1].toInt() != 0 && known != null
            val authKey = if (usePairing) {
                SessionCrypto.authKeyFromSecret(known, th)
            } else {
                val pin = "%06d".format(random.nextInt(1_000_000))
                pinShown = true
                listener.onPin(pin, "$clientName ($ip)")
                SessionCrypto.authKeyFromPin(pin, th)
            }
            Session.writeFrame(out, encode(MsgType.AUTH_START_ACK) { it.bool(!usePairing) })
            sock.soTimeout = if (usePairing) 10_000 else 120_000  // Hasta 2 minutos para escribir el PIN.
            val proofRaw = Session.readFrame(input, 1024) ?: return
            if (proofRaw.size < 33 || (proofRaw[0].toInt() and 0xFF) != MsgType.AUTH_PROOF) return
            val proof = proofRaw.copyOfRange(1, 33)
            if (!Crypto.equalsConstantTime(proof, SessionCrypto.proof(authKey, "client", th, z))) {
                synchronized(sessionLock) {
                    val f = failures[ip]
                    val count = if (f == null || System.currentTimeMillis() - f.second > 600_000) 1 else f.first + 1
                    failures[ip] = count to System.currentTimeMillis()
                }
                Session.writeFrame(out, encode(MsgType.AUTH_RESULT) {
                    it.bool(false).str(if (usePairing) "El emparejamiento no es válido. Vuelve a emparejar." else "PIN incorrecto")
                        .raw(ByteArray(32)).raw(ByteArray(16))
                })
                return
            }
            val keys = SessionCrypto.deriveKeys(z, authKey, th)
            val sessionId = Crypto.randomBytes(16)
            if (!usePairing) pairing.saveClient(cid, clientName, ip, keys.pairingSecret)
            Session.writeFrame(out, encode(MsgType.AUTH_RESULT) {
                it.bool(true).str("Conectado").raw(SessionCrypto.proof(authKey, "host", th, z)).raw(sessionId)
            })
            if (pinShown) {
                pinShown = false
                listener.onPinDone()
            }
            // Reconexión del mismo cliente (se cortó la red): reemplaza la sesión vieja.
            session?.takeIf { it.clientId == cid }?.close()
            val s = HostSession(cid, ClientInfo(clientName, emptyList(), 0, 0, 0), sock.inetAddress, out, keys)
            synchronized(sessionLock) {
                if (session != null && session!!.clientId != cid) return
                session = s
                handshaking = false
            }
            sock.soTimeout = 15_000  // El cliente hace ping cada segundo.
            listener.onSessionStarted(s.client, ip)
            try {
                s.run(input)
            } finally {
                s.close()
                synchronized(sessionLock) { if (session === s) session = null }
                listener.onSessionEnded(s.client.name)
            }
        } finally {
            synchronized(sessionLock) { handshaking = false }
            if (pinShown) listener.onPinDone()
        }
    }

    // UDP: el cliente registra su puerto con ClientHello (cifrado con la clave de medios).
    private fun mediaLoop() {
        val buf = ByteArray(2048)
        val packet = DatagramPacket(buf, buf.size)
        while (running.get()) {
            try {
                packet.setData(buf, 0, buf.size)
                media!!.receive(packet)
            } catch (_: Exception) {
                if (!running.get()) return
                continue
            }
            val s = session ?: continue
            if (packet.address != s.address || packet.length < MediaConst.PACKET_HEADER_SIZE + MediaConst.TAG_SIZE) continue
            if (buf[0].toInt() != MediaConst.TYPE_CLIENT_HELLO) continue
            s.clientHello(buf, packet.length, InetSocketAddress(packet.address, packet.port))
        }
    }

    private fun discoveryLoop() {
        val buf = ByteArray(512)
        val packet = DatagramPacket(buf, buf.size)
        while (running.get()) {
            try {
                packet.setData(buf, 0, buf.size)
                discovery!!.receive(packet)
            } catch (_: Exception) {
                if (!running.get()) return
                continue
            }
            val r = ByteReader(buf, 0, packet.length)
            if (r.u32().toInt() != Proto.DISCOVERY_MAGIC || r.u8() != 0 || !r.ok) continue
            val reply = ByteWriter().u32(Proto.DISCOVERY_MAGIC).u16(Proto.VERSION).str(describe().name).raw(deviceId)
                .u16(controlPort).u16(0).bool(session != null).toByteArray()
            runCatching { discovery?.send(DatagramPacket(reply, reply.size, packet.address, packet.port)) }
        }
    }

    // --------------------------------------------------------------------------------------- sesión

    private inner class HostSession(
        val clientId: String,
        @Volatile var client: ClientInfo,
        val address: InetAddress,
        private val out: OutputStream,
        keys: SessionCrypto.Keys,
    ) {
        private val closed = AtomicBoolean(false)
        private val outbox = LinkedBlockingQueue<ByteArray>()
        private val sender: Thread
        private val rxKey = keys.controlC2h
        private val mediaRx = AesGcm(keys.mediaC2h)
        private val videoCipher = AesGcm(keys.mediaH2c)
        private val audioCipher = AesGcm(keys.mediaH2c)
        private var videoSeq = 1L
        private var audioSeq = 1L
        private var audioPacketSeq = 0L
        @Volatile private var endpoint: InetSocketAddress? = null
        @Volatile private var epoch = 0
        private var frameId = 0L
        @Volatile var request: StreamRequest? = null
        private val videoLock = Object()
        private var paceStartNs = 0L
        private var paceBytes = 0L

        init {
            val tx = AesGcm(keys.controlH2c)
            sender = thread("lancast-host-send") {
                var counter = 0L
                try {
                    while (true) {
                        val msg = outbox.take()
                        if (msg === CLOSE) break
                        Session.writeFrame(out, tx.encrypt(Session.counterNonce(counter), null, msg))
                        counter++
                    }
                } catch (_: Exception) {
                }
                close()
            }
            send(hostInfo())
        }

        fun send(msg: ByteArray) {
            if (!closed.get()) outbox.offer(msg)
        }

        fun close() {
            if (closed.getAndSet(true)) return
            outbox.offer(CLOSE)
            runCatching { out.close() }
        }

        private fun hostInfo(): ByteArray {
            val d = describe()
            return encode(MsgType.HOST_INFO) { w ->
                w.str(d.name)
                w.u8(1)
                w.u32(1).str("Pantalla del teléfono").i32(0).i32(0).i32(d.screenWidth).i32(d.screenHeight).u32(d.refreshHz).bool(true).bool(false)
                w.u8(d.encoders.size)
                d.encoders.forEach { w.u8(it.id) }
                w.bool(d.audioAvailable).bool(d.inputAllowed).bool(d.clipboardAllowed).bool(false /* archivos */)
                w.u16(mediaPort).u16(0)
                w.bool(false /* escritorio seguro */)
                w.raw(ByteArray(6)).bool(false /* mandos */)
                w.u8(PLATFORM_ANDROID)
            }
        }

        fun run(input: DataInputStream) {
            val rx = AesGcm(rxKey)
            var counter = 0L
            while (!closed.get()) {
                val sealed = try {
                    Session.readFrame(input, 4 shl 20) ?: break
                } catch (_: SocketTimeoutException) {
                    break
                } catch (_: Exception) {
                    break
                }
                val msg = rx.decrypt(Session.counterNonce(counter++), null, sealed) ?: break
                if (msg.isNotEmpty()) handle(msg)
            }
        }

        private fun handle(msg: ByteArray) {
            val r = ByteReader(msg, 1)
            when (msg[0].toInt() and 0xFF) {
                MsgType.CLIENT_CAPS -> {
                    val name = r.str(256)
                    val n = r.u8()
                    val decoders = (0 until n).mapNotNull { Codec.of(r.u8()) }
                    val w = r.u32().toInt()
                    val h = r.u32().toInt()
                    val hz = r.u32().toInt()
                    if (r.ok) client = ClientInfo(name, decoders, w, h, hz)
                }
                MsgType.STREAM_CONFIG -> readConfig(r)?.let { cfg ->
                    if (cfg.streamIndex != 0) {
                        // Segunda ventana: el teléfono tiene una sola pantalla.
                        send(encode(MsgType.STREAM_STOPPED) { it.u8(1).str("El teléfono tiene una sola pantalla") })
                        return
                    }
                    request = cfg
                    listener.onStreamRequested(client, cfg)
                }
                MsgType.REQUEST_KEYFRAME -> listener.onKeyframeRequested()
                MsgType.PING -> {
                    val t = r.u64()
                    if (r.ok) send(encode(MsgType.PONG) { it.u64(t).u64(Session.nowUs()) })
                }
                MsgType.MOUSE_ABS -> {
                    val x = r.f32()
                    val y = r.f32()
                    if (r.ok) listener.onInput(RemoteInput.MouseAbs(x, y))
                }
                MsgType.MOUSE_BUTTON -> {
                    val b = r.u8()
                    val down = r.bool()
                    if (r.ok) listener.onInput(RemoteInput.MouseButton(b, down))
                }
                MsgType.MOUSE_WHEEL -> {
                    val d = r.i32()
                    val h = r.bool()
                    if (r.ok) listener.onInput(RemoteInput.Wheel(d, h))
                }
                MsgType.KEY -> {
                    val sc = r.u16()
                    val ext = r.bool()
                    val down = r.bool()
                    if (r.ok) listener.onInput(RemoteInput.Key(sc, ext, down))
                }
                MsgType.TEXT -> r.str(4096).let { if (r.ok) listener.onInput(RemoteInput.Text(it)) }
                MsgType.CLIPBOARD -> r.str(1 shl 20).let { if (r.ok) listener.onInput(RemoteInput.Clipboard(it)) }
                MsgType.RELEASE_ALL -> listener.onInput(RemoteInput.ReleaseAll)
                MsgType.DISCONNECT -> close()
            }
        }

        private fun readConfig(r: ByteReader): StreamRequest? {
            val monitor = r.u32().toInt()
            val maxW = r.u32().toInt()
            val maxH = r.u32().toInt()
            val fps = r.u32().toInt()
            val codec = r.u8()
            val autoBitrate = r.bool()
            val kbps = r.u32().toInt()
            r.u8()  // control de bitrate
            r.bool()  // 4:4:4
            r.bool()  // HDR
            val audio = r.bool()
            val fec = r.u8()
            val audioRate = r.u32().toInt()
            val channels = r.u8()
            var index = 0
            var enabled = true
            var pacing = 0
            if (r.ok && r.remaining >= 1) r.bool()  // baja latencia
            if (r.ok && r.remaining >= 2) {
                r.bool()  // intra-refresh
                r.bool()  // slices
                if (r.remaining >= 2) {
                    index = r.u8()
                    enabled = r.bool()
                }
                if (r.remaining >= 1) r.bool()  // ajuste automático
                if (r.remaining >= 4) pacing = r.u32().toInt()
            }
            if (!r.ok || codec > 3) return null
            return StreamRequest(monitor, maxW, maxH, fps, codec, autoBitrate, kbps, audio, fec.coerceIn(0, 50), audioRate,
                if (channels == 1) 1 else 2, index, enabled, pacing)
        }

        fun clientHello(buf: ByteArray, length: Int, from: InetSocketAddress) {
            val header = buf.copyOf(MediaConst.PACKET_HEADER_SIZE)
            mediaRx.decrypt(header, header, buf, MediaConst.PACKET_HEADER_SIZE, length - MediaConst.PACKET_HEADER_SIZE) ?: return
            if (endpoint != from) {
                endpoint = from
                listener.onKeyframeRequested()  // Nuevo destino: arranca con un cuadro completo.
            }
        }

        fun streamStarted(codec: Codec, width: Int, height: Int, fps: Int, bitrateKbps: Int, audioRate: Int, channels: Int,
                          backend: String, notes: String): Int {
            synchronized(videoLock) { epoch = (epoch + 1) and 0xFFFF }
            send(encode(MsgType.STREAM_STARTED) { w ->
                w.u16(epoch).u8(codec.id).u32(width).u32(height).u32(fps).u32(bitrateKbps).u32(1)
                w.i32(0).i32(0).i32(width).i32(height)
                w.u32(audioRate).u8(channels).str(notes)
                w.bool(false /* intra-refresh */).bool(false /* slices */).str(backend).u8(0)
                w.bool(false /* 4:4:4 */).bool(false /* HDR */)
            })
            return epoch
        }

        // Empaqueta el cuadro igual que media.cpp: bloques de hasta 100 fragmentos con paridad Reed-Solomon.
        fun sendVideo(data: ByteArray, length: Int, keyframe: Boolean, captureUs: Long, encodeUs: Long) {
            val to = endpoint ?: return
            val sock = media ?: return
            val req = request
            val fecPercent = req?.fecPercent ?: 15
            val pacing = req?.pacingMbps ?: 0
            synchronized(videoLock) {
                val id = frameId
                frameId = (frameId + 1) and 0xFFFFFFFFL
                val shardSize = MediaConst.SHARD_SIZE
                val totalShards = (length + shardSize - 1) / shardSize
                val blockCount = (totalShards + MAX_DATA_SHARDS - 1) / MAX_DATA_SHARDS
                val perBlock = (totalShards + blockCount - 1) / blockCount
                val payload = ByteArray(MediaConst.SHARD_HEADER_SIZE + shardSize)
                for (b in 0 until blockCount) {
                    val first = b * perBlock
                    val k = minOf(perBlock, totalShards - first)
                    val m = parityFor(k, fecPercent)
                    val block = ByteArray((k + m) * shardSize)
                    val offset = first * shardSize
                    System.arraycopy(data, offset, block, 0, minOf(k * shardSize, length - offset))
                    fun emit(shard: Int) {
                        val w = ByteWriter(MediaConst.SHARD_HEADER_SIZE)
                        w.u32(id).u16(b).u16(blockCount).u8(shard).u8(k).u8(m).u8(if (keyframe) MediaConst.FLAG_KEYFRAME else 0)
                            .u32(length).u64(captureUs).u32(encodeUs.coerceIn(0, 0xFFFFFFFFL)).u16(epoch).u16(0)
                        System.arraycopy(w.toByteArray(), 0, payload, 0, MediaConst.SHARD_HEADER_SIZE)
                        System.arraycopy(block, shard * shardSize, payload, MediaConst.SHARD_HEADER_SIZE, shardSize)
                        sendDatagram(sock, to, MediaConst.TYPE_VIDEO, videoSeq++, videoCipher, payload, payload.size, pacing)
                    }
                    for (s in 0 until k) emit(s)
                    if (m > 0) {
                        ReedSolomon(k, m).encode(block, shardSize)
                        for (s in k until k + m) emit(s)
                    }
                }
            }
        }

        fun sendAudio(pcm: ShortArray, frames: Int, rate: Int, channels: Int) {
            val to = endpoint ?: return
            val sock = media ?: return
            if (request?.audio == false) return
            val maxFrames = 600 / channels  // 1200 bytes de PCM por datagrama, como el host de Windows.
            var off = 0
            while (off < frames) {
                val n = minOf(maxFrames, frames - off)
                val w = ByteWriter(20 + n * channels * 2)
                w.u32(++audioPacketSeq).u64(Session.nowUs()).u32(rate).u8(channels).u16(n)
                for (i in 0 until n * channels) w.u16(pcm[off * channels + i].toInt() and 0xFFFF)
                val bytes = w.toByteArray()
                synchronized(audioCipher) { sendDatagram(sock, to, MediaConst.TYPE_AUDIO, audioSeq++, audioCipher, bytes, bytes.size, 0) }
                off += n
            }
        }

        // [cabecera 12 B (nonce y AAD)][carga cifrada][tag]. Con ritmo, no se envía más rápido que pacing Mbps.
        private fun sendDatagram(sock: DatagramSocket, to: InetSocketAddress, type: Int, seq: Long, cipher: AesGcm, payload: ByteArray,
                                 length: Int, pacingMbps: Int) {
            val header = packetHeader(type, seq)
            val sealed = cipher.encrypt(header, header, payload, 0, length)
            val datagram = ByteArray(header.size + sealed.size)
            System.arraycopy(header, 0, datagram, 0, header.size)
            System.arraycopy(sealed, 0, datagram, header.size, sealed.size)
            if (pacingMbps > 0) {
                val now = System.nanoTime()
                if (paceStartNs == 0L || now - paceStartNs > 50_000_000L) {
                    paceStartNs = now
                    paceBytes = 0
                }
                val due = paceStartNs + paceBytes * 8 * 1000 / pacingMbps
                if (due > now) LockSupport.parkNanos(due - now)
                paceBytes += datagram.size
            }
            try {
                sock.send(DatagramPacket(datagram, datagram.size, to))
            } catch (_: Exception) {
            }
        }
    }

    companion object {
        const val DEFAULT_MEDIA_PORT = 47998
        const val PLATFORM_ANDROID = 1
        private const val MAX_DATA_SHARDS = 100
        private val CLOSE = ByteArray(0)

        fun parityFor(dataShards: Int, fecPercent: Int): Int {
            if (fecPercent <= 0 || dataShards <= 0) return 0
            val m = maxOf((dataShards * fecPercent + 99) / 100, 1)
            return minOf(m, MediaConst.MAX_TOTAL_SHARDS - dataShards)
        }

        private fun openServer(port: Int): ServerSocket {
            var last: Exception? = null
            for (p in port until port + 10) {
                try {
                    return ServerSocket().apply {
                        reuseAddress = true
                        bind(InetSocketAddress(p))
                    }
                } catch (e: Exception) {
                    last = e
                }
            }
            throw last ?: IllegalStateException("Sin puerto")
        }

        private fun openUdp(port: Int): DatagramSocket {
            var last: Exception? = null
            for (p in port until port + 10) {
                try {
                    return DatagramSocket(p)
                } catch (e: Exception) {
                    last = e
                }
            }
            throw last ?: IllegalStateException("Sin puerto")
        }

        private fun thread(name: String, body: () -> Unit) = Thread(body, name).apply {
            isDaemon = true
            start()
        }
    }
}
