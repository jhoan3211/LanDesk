package com.lancast.core

import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

// El cliente Kotlin contra el host real de LanCast (C++): emparejar con PIN, recibir video y reconectar sin PIN.
// Requiere build/tests/Release/lancast-test-host.exe (CMake); si no existe, la prueba se omite.
class InteropTest {
    private val port = 48900

    private class Listener : SessionListener {
        val needPin = AtomicBoolean(false)
        val connected = CountDownLatch(1)
        val stream = AtomicReference<StreamStarted?>(null)
        val keyframes = AtomicInteger(0)
        val frames = AtomicInteger(0)
        val firstKey = AtomicReference<ByteArray?>(null)
        val gotKeyframe = CountDownLatch(1)
        val closed = AtomicReference<String?>(null)
        var onPin: (() -> Unit)? = null

        override fun onNeedPin(hostName: String) {
            needPin.set(true)
            onPin?.invoke()
        }

        override fun onConnected(hostName: String) = connected.countDown()
        override fun onStreamStarted(stream: StreamStarted) = this.stream.set(stream)
        override fun onVideoFrame(frame: AssembledFrame, arrivalUs: Long) {
            frames.incrementAndGet()
            if (frame.info.keyframe) {
                keyframes.incrementAndGet()
                firstKey.compareAndSet(null, frame.data)
                gotKeyframe.countDown()
            }
        }

        override fun onClosed(reason: String, retryable: Boolean) = closed.set(reason)
    }

    @Test
    fun pairsStreamsAndReconnects() {
        val exe = File(System.getProperty("lancast.testHost") ?: "")
        if (!exe.isFile) {
            println("Se omite: no existe ${exe.path} (compila con CMake primero)")
            return
        }
        val dir = File(System.getProperty("java.io.tmpdir"), "lancast-interop-${System.nanoTime()}")
        val proc = ProcessBuilder(exe.path, port.toString(), dir.path).redirectErrorStream(true).start()
        val lines = LinkedBlockingQueue<String>()
        Thread {
            proc.inputStream.bufferedReader().forEachLine { lines.put(it) }
        }.apply { isDaemon = true }.start()
        try {
            fun waitLine(prefix: String, seconds: Long): String? {
                val deadline = System.currentTimeMillis() + seconds * 1000
                while (System.currentTimeMillis() < deadline) {
                    val l = lines.poll(200, TimeUnit.MILLISECONDS) ?: continue
                    if (l.startsWith("ERROR")) error("El host de prueba falló: $l")
                    if (l.startsWith(prefix)) return l
                }
                return null
            }
            assertNotNull(waitLine("READY", 20), "El host de prueba no arrancó")

            val pairing = MemoryPairingStore()
            val deviceId = Crypto.randomBytes(16)
            val caps = ClientCaps("Celular-Prueba", listOf(Codec.H264, Codec.HEVC), 1280, 720, 60)
            val config = StreamConfig(monitorId = 0, maxWidth = 1280, maxHeight = 720, fps = 60, audio = true)

            // 1) Primera conexión: PIN.
            val l1 = Listener()
            val s1 = Session(deviceId, "Celular-Prueba", pairing, l1)
            l1.onPin = { Thread { waitLine("PIN ", 10)?.let { s1.submitPin(it.substring(4).trim()) } }.start() }
            s1.connect("127.0.0.1", port, caps, config)
            assertTrue(l1.connected.await(30, TimeUnit.SECONDS), "No conectó: ${l1.closed.get()}")
            assertTrue(l1.needPin.get(), "La primera vez debe pedir PIN")
            assertTrue(l1.gotKeyframe.await(10, TimeUnit.SECONDS), "No llegó ningún keyframe")
            Thread.sleep(1500)
            val st = l1.stream.get()
            assertNotNull(st)
            val key = l1.firstKey.get()!!
            val annexB = (key[0].toInt() == 0 && key[1].toInt() == 0 && (key[2].toInt() == 1 || (key[2].toInt() == 0 && key[3].toInt() == 1)))
            println("  Stream: ${st.codec.label} ${st.width}x${st.height} @ ${st.fps} fps, ${st.encoderBackend}, " +
                "${l1.frames.get()} cuadros (${l1.keyframes.get()} keyframes), keyframe ${key.size} bytes, RTT ${"%.2f".format(s1.stats.rttMs)} ms")
            assertTrue(annexB, "El bitstream debe venir en formato Annex-B")
            assertTrue(st.width <= 1280 && st.height <= 720)

            // Muchos mensajes desde varios hilos a la vez (como el toque y el teclado del celular): el canal cifrado
            // no debe desincronizarse; si pasara, el host cortaría la sesión.
            val before = l1.frames.get()
            val senders = (1..4).map { t ->
                Thread {
                    repeat(150) { i ->
                        s1.send(Messages.mouseAbs((i % 100) / 100f, t / 5f))
                        if (i % 10 == 0) s1.send(Messages.mouseWheel(0))
                    }
                }.apply { start() }
            }
            senders.forEach { it.join() }
            Thread.sleep(2500)
            assertEquals(null, l1.closed.get(), "La sesión no debe cortarse al enviar desde varios hilos")
            s1.requestKeyframe()
            Thread.sleep(800)
            assertTrue(l1.frames.get() > before, "Debe seguir llegando video")
            println("  Tras 660 mensajes desde 4 hilos: sesión activa, RTT ${"%.2f".format(s1.stats.rttMs)} ms")
            s1.close()
            Thread.sleep(500)

            // 2) Reconexión: ya emparejado, sin PIN.
            val l2 = Listener()
            val s2 = Session(deviceId, "Celular-Prueba", pairing, l2)
            s2.connect("127.0.0.1", port, caps, config)
            assertTrue(l2.connected.await(15, TimeUnit.SECONDS), "No reconectó: ${l2.closed.get()}")
            assertFalse(l2.needPin.get(), "Emparejado: no debe pedir PIN")
            assertTrue(l2.gotKeyframe.await(10, TimeUnit.SECONDS))
            s2.close()

            // 3) Descubrimiento en la red local (el host responde a broadcast y a localhost).
            val found = Session.discover(1500, listOf(java.net.InetAddress.getByName("127.0.0.1")))
            println("  Descubiertos: ${found.map { "${it.name} (${it.address}:${it.controlPort})" }}")
            // Si LanCast está abierto en este PC, también escucha el puerto de descubrimiento y puede responder él.
            assertTrue(found.isNotEmpty(), "Ningún host respondió al descubrimiento")
            found.firstOrNull { it.name == "Host-Interop" }?.let { assertEquals(port, it.controlPort) }
        } finally {
            proc.outputStream.close()
            proc.waitFor(5, TimeUnit.SECONDS)
            proc.destroyForcibly()
            dir.deleteRecursively()
        }
    }
}
