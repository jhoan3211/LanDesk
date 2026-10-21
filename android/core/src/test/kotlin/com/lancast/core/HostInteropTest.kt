package com.lancast.core

import java.io.File
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.random.Random
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

// El host del teléfono (Kotlin) contra el cliente real del PC (C++, lancast-test-client.exe): emparejar con PIN,
// recibir video y mandar control. Si el ejecutable no existe (compilar con CMake), la prueba se omite.
class HostInteropTest {
    @Test
    fun pcClientControlsPhoneHost() {
        val exe = File(System.getProperty("lancast.testHost") ?: "").resolveSibling("lancast-test-client.exe")
        if (!exe.isFile) {
            println("Se omite: no existe ${exe.path} (compila con CMake primero)")
            return
        }
        val pins = LinkedBlockingQueue<String>()
        val inputs = CopyOnWriteArrayList<RemoteInput>()
        val streaming = AtomicBoolean(false)
        val sentFrames = java.util.concurrent.atomic.AtomicInteger(0)
        var server: HostServer? = null
        val clients = HashMap<String, ByteArray>()
        val keyRequested = AtomicBoolean(true)
        val listener = object : HostListener {
            override fun onPin(pin: String, requester: String) {
                pins.put(pin)
            }

            override fun onStreamRequested(client: ClientInfo, request: StreamRequest) {
                // El teléfono decide su tamaño (pantalla vertical) aunque el PC pida 2560x1440 a 165 fps.
                server!!.streamStarted(Codec.HEVC, 1080, 2340, minOf(request.fps, 60), request.bitrateKbps, 0, 0, "Prueba")
                streaming.set(true)
            }

            override fun onKeyframeRequested() = keyRequested.set(true)
            override fun onInput(input: RemoteInput) {
                inputs.add(input)
            }
        }
        val store = object : HostPairingStore {
            override fun clientSecret(clientId: String) = clients[clientId]
            override fun saveClient(clientId: String, name: String, address: String, secret: ByteArray) {
                clients[clientId] = secret
            }
        }
        server = HostServer(Crypto.randomBytes(16), store, listener) {
            HostDescription("Teléfono-Interop", 1080, 2340, 120, listOf(Codec.H264, Codec.HEVC), audioAvailable = false,
                inputAllowed = true, clipboardAllowed = true)
        }
        assertTrue(server.start(48720, 48734))
        val dir = File(System.getProperty("java.io.tmpdir"), "lancast-host-interop-${System.nanoTime()}")
        val proc = ProcessBuilder(exe.path, "127.0.0.1", server.controlPort.toString(), dir.path).redirectErrorStream(true).start()
        val lines = LinkedBlockingQueue<String>()
        Thread { proc.inputStream.bufferedReader().forEachLine { lines.put(it) } }.apply { isDaemon = true }.start()
        val stdin = proc.outputStream.bufferedWriter()
        // Cuadros "de video" a 60 fps mientras el cliente está conectado (el cliente no decodifica en esta prueba).
        val feeder = Thread {
            val rnd = Random(3)
            while (!Thread.currentThread().isInterrupted) {
                if (streaming.get()) {
                    val key = keyRequested.getAndSet(false)
                    val size = if (key) 120_000 else 8_000 + rnd.nextInt(30_000)
                    val data = ByteArray(size).also { rnd.nextBytes(it) }
                    server.sendVideo(data, size, key, Session.nowUs(), 2000)
                    sentFrames.incrementAndGet()
                }
                try {
                    Thread.sleep(16)
                } catch (_: InterruptedException) {
                    break
                }
            }
        }.apply { isDaemon = true; start() }
        try {
            fun waitLine(prefix: String, seconds: Long): String? {
                val deadline = System.currentTimeMillis() + seconds * 1000
                while (System.currentTimeMillis() < deadline) {
                    val l = lines.poll(200, TimeUnit.MILLISECONDS) ?: continue
                    println("  [cliente PC] $l")
                    if (l.startsWith("ERROR")) error("El cliente de PC falló: $l")
                    if (l.startsWith(prefix)) return l
                }
                return null
            }
            assertNotNull(waitLine("NEED_PIN", 20), "El cliente de PC no pidió PIN")
            val pin = pins.poll(5, TimeUnit.SECONDS)
            assertNotNull(pin)
            stdin.write("PIN $pin\n")
            stdin.flush()
            assertNotNull(waitLine("CONNECTED", 20), "El cliente de PC no conectó")
            val host = waitLine("HOST", 5)
            assertNotNull(host)
            assertTrue(host.contains("PLATFORM 1") && host.contains("1080x2340"), "HostInfo del teléfono: $host")
            val frames = waitLine("FRAMES", 15)
            assertNotNull(frames)
            val parts = frames.split(' ')
            val got = parts[1].toInt()
            val keys = parts[3].toInt()
            println("  El PC recibió $got cuadros ($keys keyframes) en 4 s; el teléfono mandó ${sentFrames.get()}")
            assertTrue(got > 150, "Muy pocos cuadros: $got")
            assertTrue(keys >= 1, "Sin keyframe")
            assertNotNull(waitLine("INPUT_SENT", 10))
            Thread.sleep(800)
            assertTrue(RemoteInput.MouseAbs(0.5f, 0.25f) in inputs, "Control del PC: $inputs")
            assertTrue(RemoteInput.MouseButton(0, true) in inputs && RemoteInput.MouseButton(0, false) in inputs)
            assertTrue(RemoteInput.Text("Desde el PC: ñandú") in inputs)
            assertEquals(1, clients.size, "El PC quedó emparejado")
        } finally {
            feeder.interrupt()
            stdin.close()
            proc.waitFor(5, TimeUnit.SECONDS)
            proc.destroyForcibly()
            server.stop()
            dir.deleteRecursively()
        }
    }
}
