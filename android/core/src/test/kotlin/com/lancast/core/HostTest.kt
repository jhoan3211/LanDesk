package com.lancast.core

import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlin.random.Random
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

// El host del teléfono (Kotlin) contra el cliente del teléfono (Kotlin) por loopback: emparejar con PIN, recibir
// cuadros completos (con FEC), mandar el control y reconectar sin PIN.
class HostTest {
    private class HostSide : HostListener, HostPairingStore {
        val pin = AtomicReference<String?>(null)
        val pinLatch = CountDownLatch(1)
        val requested = LinkedBlockingQueue<StreamRequest>()
        val keyframes = LinkedBlockingQueue<Long>()
        val inputs = CopyOnWriteArrayList<RemoteInput>()
        val clients = HashMap<String, ByteArray>()
        var server: HostServer? = null
        var clientName = ""

        override fun onPin(pin: String, requester: String) {
            this.pin.set(pin)
            pinLatch.countDown()
        }

        override fun onSessionStarted(client: ClientInfo, address: String) {
            clientName = client.name
        }

        override fun onStreamRequested(client: ClientInfo, request: StreamRequest) {
            requested.put(request)
            server!!.streamStarted(Codec.H264, 640, 480, 60, request.bitrateKbps, 0, 0, "Prueba")
        }

        override fun onKeyframeRequested() {
            keyframes.put(System.nanoTime())
        }

        override fun onInput(input: RemoteInput) {
            inputs.add(input)
        }

        override fun clientSecret(clientId: String) = synchronized(clients) { clients[clientId] }
        override fun saveClient(clientId: String, name: String, address: String, secret: ByteArray) {
            synchronized(clients) { clients[clientId] = secret }
        }
    }

    private class ClientSide : SessionListener {
        val connected = CountDownLatch(1)
        val frames = LinkedBlockingQueue<AssembledFrame>()
        val info = AtomicReference<HostInfo?>(null)
        val closed = AtomicReference<String?>(null)
        var onPin: (() -> Unit)? = null
        var needPin = false

        override fun onNeedPin(hostName: String) {
            needPin = true
            onPin?.invoke()
        }

        override fun onConnected(hostName: String) = connected.countDown()
        override fun onHostInfo(info: HostInfo) = this.info.set(info)
        override fun onVideoFrame(frame: AssembledFrame, arrivalUs: Long) {
            frames.put(frame)
        }

        override fun onClosed(reason: String, retryable: Boolean) = closed.set(reason)
    }

    @Test
    fun phoneHostServesPhoneClient() {
        val host = HostSide()
        val hostId = Crypto.randomBytes(16)
        val server = HostServer(hostId, host, host) {
            HostDescription("Teléfono-Prueba", 1080, 2340, 120, listOf(Codec.H264, Codec.HEVC), audioAvailable = true,
                inputAllowed = true, clipboardAllowed = true)
        }
        host.server = server
        assertTrue(server.start(48700, 48714), "El host no abrió los puertos")
        try {
            val pairing = MemoryPairingStore()
            val deviceId = Crypto.randomBytes(16)
            val caps = ClientCaps("Cliente-Prueba", listOf(Codec.H264), 1280, 720, 60)
            val config = StreamConfig(maxWidth = 1280, maxHeight = 720, fps = 60, bitrateKbps = 8000, fecPercent = 20)

            // 1) PIN.
            val c1 = ClientSide()
            val s1 = Session(deviceId, "Cliente-Prueba", pairing, c1)
            c1.onPin = {
                Thread {
                    assertTrue(host.pinLatch.await(5, TimeUnit.SECONDS))
                    s1.submitPin(host.pin.get()!!)
                }.start()
            }
            s1.connect("127.0.0.1", server.controlPort, caps, config)
            assertTrue(c1.connected.await(20, TimeUnit.SECONDS), "No conectó: ${c1.closed.get()}")
            assertTrue(c1.needPin, "La primera vez pide PIN")
            val req = host.requested.poll(5, TimeUnit.SECONDS)
            assertNotNull(req, "El host no recibió el pedido de stream")
            assertEquals(8000, req.bitrateKbps)
            assertEquals(20, req.fecPercent)
            val infoUntil = System.currentTimeMillis() + 5000
            while (c1.info.get() == null && System.currentTimeMillis() < infoUntil) Thread.sleep(20)
            val info = c1.info.get()
            assertNotNull(info, "No llegó HostInfo")
            assertEquals(HostServer.PLATFORM_ANDROID, info.platform)
            assertEquals(2340, info.monitors.first().rect.height)
            assertEquals("Cliente-Prueba", host.clientName)

            // El cliente registra su puerto UDP: el host pide un keyframe y empieza a mandar.
            assertNotNull(host.keyframes.poll(5, TimeUnit.SECONDS), "El cliente no registró su puerto UDP")
            // El aviso de stream va por TCP y el video por UDP (otro hilo): se espera a que el cliente lo procese.
            val until = System.currentTimeMillis() + 5000
            while (s1.stream == null && System.currentTimeMillis() < until) Thread.sleep(20)
            assertNotNull(s1.stream, "El cliente no recibió el aviso de stream")
            val rnd = Random(7)
            val sent = listOf(150_000, 900, 40_000, 260_000, 3).mapIndexed { i, size ->
                ByteArray(size).also { rnd.nextBytes(it); if (size > 4) { it[0] = 0; it[1] = 0; it[2] = 0; it[3] = 1 } } to (i == 0)
            }
            for ((data, key) in sent) {
                server.sendVideo(data, data.size, key, Session.nowUs(), 1500)
                Thread.sleep(20)
            }
            for ((data, _) in sent) {
                val got = c1.frames.poll(5, TimeUnit.SECONDS)
                assertNotNull(got, "Faltó un cuadro de ${data.size} bytes")
                assertContentEquals(data, got.data, "Cuadro distinto (${data.size} bytes)")
            }
            println("  Video: 5 cuadros (3 B a 260 KB, en bloques con paridad) llegaron idénticos")

            // Control remoto.
            s1.send(Messages.mouseAbs(0.25f, 0.75f))
            s1.send(Messages.mouseButton(MouseButtonId.LEFT, true))
            s1.send(Messages.mouseButton(MouseButtonId.LEFT, false))
            s1.send(Messages.mouseWheel(-120))
            s1.send(Messages.text("Hola ñandú"))
            s1.send(Messages.key(0x0E, false, true))
            Thread.sleep(800)
            val inputs = host.inputs.toList()
            assertTrue(RemoteInput.MouseAbs(0.25f, 0.75f) in inputs, "Llegó el mouse: $inputs")
            assertTrue(RemoteInput.MouseButton(0, true) in inputs && RemoteInput.MouseButton(0, false) in inputs)
            assertTrue(RemoteInput.Wheel(-120, false) in inputs)
            assertTrue(RemoteInput.Text("Hola ñandú") in inputs)
            assertTrue(RemoteInput.Key(0x0E, false, true) in inputs)
            s1.close()
            Thread.sleep(600)
            assertTrue(!server.hasSession, "La sesión debe terminar al desconectar")

            // 2) Reconexión ya emparejados: sin PIN.
            val c2 = ClientSide()
            val s2 = Session(deviceId, "Cliente-Prueba", pairing, c2)
            s2.connect("127.0.0.1", server.controlPort, caps, config)
            assertTrue(c2.connected.await(10, TimeUnit.SECONDS), "No reconectó: ${c2.closed.get()}")
            assertTrue(!c2.needPin, "Emparejado: no pide PIN")
            s2.close()
            Thread.sleep(600)

            // 3) El router le dio otra IP al teléfono: el cliente lo busca por su id y entra sin PIN.
            val visible = Session.discover(800, listOf(java.net.InetAddress.getByName("127.0.0.1"))).any { it.deviceId == hostId.toHex() }
            if (!visible) {
                println("  Se omite el cambio de IP: el descubrimiento no llega a este host (¿LanCast abierto en este PC?)")
            } else {
                pairing.updateAddress(hostId.toHex(), "192.0.2.123")
                val c3 = ClientSide()
                val s3 = Session(deviceId, "Cliente-Prueba", pairing, c3) { listOf(java.net.InetAddress.getByName("127.0.0.1")) }
                val t0 = System.nanoTime()
                s3.connect("192.0.2.123", server.controlPort, caps, config)
                assertTrue(c3.connected.await(15, TimeUnit.SECONDS), "No encontró al host por su id: ${c3.closed.get()}")
                assertTrue(!c3.needPin, "Emparejado: no pide PIN aunque cambió la IP")
                println("  IP cambiada: encontrado por su id y conectado sin PIN en %.1f s".format((System.nanoTime() - t0) / 1e9))
                assertEquals(hostId.toHex(), pairing.hostIdAt(s3.hostAddress?.hostAddress ?: ""))
                s3.close()
            }
        } finally {
            server.stop()
        }
    }
}
