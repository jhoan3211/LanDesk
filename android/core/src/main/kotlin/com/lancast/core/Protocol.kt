package com.lancast.core

// Mensajes del canal de control. Mismo formato que lancast-core/protocol.hpp/.cpp del host: cada mensaje es
// [u8 tipo][cuerpo]; los campos "opcionales" van al final y se leen solo si quedan bytes (compatibilidad).

object Proto {
    const val MAGIC = 0x534E434C           // "LCNS"
    const val VERSION = 2
    const val DISCOVERY_MAGIC = 0x4353434C  // "LCSC"
    const val DEFAULT_CONTROL_PORT = 47984
    const val DISCOVERY_PORT = 47999
}

object MsgType {
    const val HELLO = 1
    const val HELLO_ACK = 2
    const val AUTH_START = 3
    const val AUTH_START_ACK = 4
    const val AUTH_PROOF = 5
    const val AUTH_RESULT = 6
    const val CLIENT_CAPS = 10
    const val HOST_INFO = 11
    const val STREAM_CONFIG = 12
    const val STREAM_STARTED = 13
    const val STREAM_ERROR = 14
    const val REQUEST_KEYFRAME = 15
    const val PING = 16
    const val PONG = 17
    const val MOUSE_ABS = 20
    const val MOUSE_REL = 21
    const val MOUSE_BUTTON = 22
    const val MOUSE_WHEEL = 23
    const val KEY = 24
    const val RELEASE_ALL = 25
    const val CLIPBOARD = 26
    const val CURSOR_SHAPE = 27
    const val CURSOR_POS = 28
    const val DISCONNECT = 30
    const val STREAM_STOPPED = 33
    const val SECURE_ATTENTION = 34
    const val TEXT = 35
    const val GAMEPAD = 36
    const val FILE_HELLO = 40
    const val FILE_OFFER = 41
    const val FILE_ACCEPT = 42
    const val FILE_REJECT = 43
    const val FILE_CHUNK = 44
    const val FILE_DONE = 45
    const val FILE_RESULT = 46
    const val FILE_CANCEL = 47
}

enum class Codec(val id: Int, val label: String) {
    H264(0, "H.264"), HEVC(1, "HEVC"), AV1(2, "AV1");

    companion object {
        fun of(id: Int): Codec? = entries.firstOrNull { it.id == id }
    }
}

// Elección del cliente: 0 = automático, 1 = H.264, 2 = HEVC, 3 = AV1.
enum class CodecChoice(val id: Int) { AUTO(0), H264(1), HEVC(2), AV1(3) }

enum class MouseButtonId(val id: Int) { LEFT(0), RIGHT(1), MIDDLE(2) }

data class Rect(val x: Int, val y: Int, val width: Int, val height: Int)

fun encode(type: Int, body: (ByteWriter) -> Unit = {}): ByteArray {
    val w = ByteWriter()
    w.u8(type)
    body(w)
    return w.toByteArray()
}

class Hello(val deviceId: ByteArray, val deviceName: String, val publicKey: ByteArray, val nonce: ByteArray) {
    fun encode(): ByteArray = encode(MsgType.HELLO) {
        it.u32(Proto.MAGIC).u16(Proto.VERSION).raw(deviceId).str(deviceName).raw(publicKey).raw(nonce)
    }
}

data class HelloAck(
    val version: Int,
    val deviceId: ByteArray,
    val deviceName: String,
    val publicKey: ByteArray,
    val hasPairing: Boolean,
    val accepted: Boolean,
    val message: String,
) {
    companion object {
        fun read(r: ByteReader): HelloAck? {
            val version = r.u16()
            val id = r.raw(16)
            val name = r.str(256)
            val pub = r.raw(64)
            r.raw(32)  // nonce
            val hasPairing = r.bool()
            val accepted = r.bool()
            val message = r.str(4096)
            return if (r.ok) HelloAck(version, id, name, pub, hasPairing, accepted, message) else null
        }
    }
}

data class AuthResult(val ok: Boolean, val message: String, val mac: ByteArray, val sessionId: ByteArray) {
    companion object {
        fun read(r: ByteReader): AuthResult? {
            val ok = r.bool()
            val message = r.str(4096)
            val mac = r.raw(32)
            val session = r.raw(16)
            return if (r.ok) AuthResult(ok, message, mac, session) else null
        }
    }
}

data class ClientCaps(
    val name: String,
    val decoders: List<Codec>,
    val displayWidth: Int,
    val displayHeight: Int,
    val displayRefresh: Int,
    val supportsSlices: Boolean = true,
    val supports444: Boolean = false,
    val supportsHdr: Boolean = false,
) {
    fun encode(): ByteArray = encode(MsgType.CLIENT_CAPS) { w ->
        w.str(name)
        w.u8(decoders.size)
        decoders.forEach { w.u8(it.id) }
        w.u32(displayWidth).u32(displayHeight).u32(displayRefresh)
        w.bool(supportsSlices).bool(supports444).bool(supportsHdr)
    }
}

data class RemoteMonitor(val id: Int, val name: String, val rect: Rect, val refreshHz: Int, val primary: Boolean)

data class HostInfo(
    val hostName: String,
    val monitors: List<RemoteMonitor>,
    val encoders: List<Codec>,
    val audioAvailable: Boolean,
    val inputAllowed: Boolean,
    val clipboardAllowed: Boolean,
    val filesAllowed: Boolean,
    val mediaPort: Int,
    val filePort: Int,
    val secureDesktop: Boolean,
    val mac: ByteArray = ByteArray(6),       // 1.7+: MAC del PC para Wake-on-LAN (ceros = no se sabe).
    val gamepadAvailable: Boolean = false,   // 1.7+: el PC tiene el driver de mandos virtuales (ViGEmBus).
) {
    val hasMac: Boolean get() = mac.any { it.toInt() != 0 }

    companion object {
        fun read(r: ByteReader): HostInfo? {
            val name = r.str(256)
            val n = r.u8()
            val monitors = ArrayList<RemoteMonitor>()
            for (i in 0 until n) {
                if (!r.ok) break
                val id = r.u32().toInt()
                val mname = r.str(256)
                val rect = Rect(r.i32(), r.i32(), r.i32(), r.i32())
                val hz = r.u32().toInt()
                val primary = r.bool()
                r.bool()  // hdr
                monitors += RemoteMonitor(id, mname, rect, hz, primary)
            }
            val encoders = readCodecs(r)
            val audio = r.bool()
            val input = r.bool()
            val clipboard = r.bool()
            val files = r.bool()
            val mediaPort = r.u16()
            val filePort = r.u16()
            val secure = if (r.remaining >= 1) r.bool() else false
            var mac = ByteArray(6)
            var gamepad = false
            if (r.remaining >= 7) {
                mac = r.raw(6)
                gamepad = r.bool()
            }
            return if (r.ok) HostInfo(name, monitors, encoders, audio, input, clipboard, files, mediaPort, filePort, secure, mac, gamepad)
            else null
        }
    }
}

private fun readCodecs(r: ByteReader): List<Codec> {
    val n = r.u8()
    val out = ArrayList<Codec>()
    for (i in 0 until n) {
        if (!r.ok) break
        Codec.of(r.u8())?.let { out += it }
    }
    return out
}

data class StreamConfig(
    val monitorId: Int = 1,
    val maxWidth: Int = 1920,
    val maxHeight: Int = 1080,
    val fps: Int = 60,
    val codec: CodecChoice = CodecChoice.AUTO,
    val autoBitrate: Boolean = true,
    val bitrateKbps: Int = 30000,
    val audio: Boolean = true,
    val fecPercent: Int = 15,
    val audioRate: Int = 48000,
    val audioChannels: Int = 2,
    val lowLatency: Boolean = true,
    val slices: Boolean = true,
    val adaptive: Boolean = true,
    val pacingMbps: Int = 0,  // 1.7+: ritmo máximo de envío del host (0 = sin límite); ayuda por Wi-Fi.
) {
    fun encode(): ByteArray = encode(MsgType.STREAM_CONFIG) { w ->
        w.u32(monitorId).u32(maxWidth).u32(maxHeight).u32(fps).u8(codec.id).bool(autoBitrate)
        w.u32(bitrateKbps).u8(0 /* CBR */).bool(false /* 4:4:4 */).bool(false /* HDR */).bool(audio).u8(fecPercent)
        w.u32(audioRate).u8(audioChannels)
        w.bool(lowLatency)
        w.bool(false /* intra-refresh */).bool(slices)
        w.u8(0 /* stream principal */).bool(true /* activo */)
        w.bool(adaptive)
        w.u32(pacingMbps)
    }
}

data class StreamStarted(
    val epoch: Int,
    val codec: Codec,
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrateKbps: Long,
    val monitorId: Int,
    val source: Rect,
    val audioRate: Int,
    val audioChannels: Int,
    val notes: String,
    val intraRefresh: Boolean,
    val slices: Boolean,
    val encoderBackend: String,
    val streamIndex: Int,
    val chroma444: Boolean,
    val hdr: Boolean,
) {
    companion object {
        fun read(r: ByteReader): StreamStarted? {
            val epoch = r.u16()
            val codec = Codec.of(r.u8())
            val w = r.u32().toInt()
            val h = r.u32().toInt()
            val fps = r.u32().toInt()
            val kbps = r.u32()
            val monitor = r.u32().toInt()
            val source = Rect(r.i32(), r.i32(), r.i32(), r.i32())
            val audioRate = r.u32().toInt()
            val audioChannels = r.u8()
            val notes = r.str(4096)
            var intra = false
            var slices = false
            var backend = ""
            var index = 0
            if (r.ok && r.remaining >= 2) {
                intra = r.bool()
                slices = r.bool()
                if (r.remaining >= 4) backend = r.str(128)
                if (r.ok && r.remaining >= 1) index = r.u8()
            }
            var c444 = false
            var hdr = false
            if (r.ok && r.remaining >= 2) {
                c444 = r.bool()
                hdr = r.bool()
            }
            if (!r.ok || codec == null) return null
            return StreamStarted(epoch, codec, w, h, fps, kbps, monitor, source, audioRate, audioChannels, notes, intra, slices,
                backend, index, c444, hdr)
        }
    }
}

data class CursorPos(val visible: Boolean, val x: Float, val y: Float, val streamIndex: Int) {
    companion object {
        fun read(r: ByteReader): CursorPos? {
            val v = r.bool()
            val x = r.f32()
            val y = r.f32()
            val idx = if (r.ok && r.remaining >= 1) r.u8() else 0
            return if (r.ok) CursorPos(v, x, y, idx) else null
        }
    }
}

// Forma del cursor tal como la entrega DXGI (1 = monocromo AND+XOR, 2 = color BGRA, 4 = color con máscara XOR).
data class CursorShape(val type: Int, val width: Int, val height: Int, val pitch: Int, val hotX: Int, val hotY: Int, val data: ByteArray) {
    companion object {
        fun read(r: ByteReader): CursorShape? {
            val type = r.u8()
            val w = r.u32().toInt()
            val h = r.u32().toInt()
            val pitch = r.u32().toInt()
            val hx = r.i32()
            val hy = r.i32()
            val data = r.blob(4 shl 20)
            if (!r.ok || w > 512 || h > 1024 || data.size < pitch.toLong() * h) return null
            return CursorShape(type, w, h, pitch, hx, hy, data)
        }
    }
}

// Mando en formato XInput: botones XUSB (A = 0x1000...), gatillos 0..255, ejes -32768..32767.
data class GamepadState(
    val index: Int = 0,
    val connected: Boolean = true,
    val buttons: Int = 0,
    val leftTrigger: Int = 0,
    val rightTrigger: Int = 0,
    val leftX: Int = 0,
    val leftY: Int = 0,
    val rightX: Int = 0,
    val rightY: Int = 0,
) {
    companion object {
        const val DPAD_UP = 0x0001
        const val DPAD_DOWN = 0x0002
        const val DPAD_LEFT = 0x0004
        const val DPAD_RIGHT = 0x0008
        const val START = 0x0010
        const val BACK = 0x0020
        const val LEFT_THUMB = 0x0040
        const val RIGHT_THUMB = 0x0080
        const val LEFT_SHOULDER = 0x0100
        const val RIGHT_SHOULDER = 0x0200
        const val GUIDE = 0x0400
        const val A = 0x1000
        const val B = 0x2000
        const val X = 0x4000
        const val Y = 0x8000
    }
}

data class DiscoveredHost(val name: String, val deviceId: String, val address: String, val controlPort: Int, val busy: Boolean)

object Messages {
    fun ping(clientTimeUs: Long) = encode(MsgType.PING) { it.u64(clientTimeUs).u64(0) }
    fun mouseAbs(x: Float, y: Float) = encode(MsgType.MOUSE_ABS) { it.f32(x).f32(y).u8(0) }
    fun mouseRel(dx: Int, dy: Int) = encode(MsgType.MOUSE_REL) { it.i32(dx).i32(dy) }
    fun mouseButton(button: MouseButtonId, down: Boolean) = encode(MsgType.MOUSE_BUTTON) { it.u8(button.id).bool(down) }
    fun mouseWheel(delta: Int, horizontal: Boolean = false) = encode(MsgType.MOUSE_WHEEL) { it.i32(delta).bool(horizontal) }
    fun key(scancode: Int, extended: Boolean, down: Boolean) = encode(MsgType.KEY) { it.u16(scancode).bool(extended).bool(down) }
    fun text(text: String) = encode(MsgType.TEXT) { it.str(text) }
    fun clipboard(text: String) = encode(MsgType.CLIPBOARD) { it.str(text) }
    fun releaseAll() = encode(MsgType.RELEASE_ALL)
    fun gamepad(p: GamepadState) = encode(MsgType.GAMEPAD) { w ->
        w.u8(p.index).bool(p.connected).u16(p.buttons).u8(p.leftTrigger).u8(p.rightTrigger)
        w.u16(p.leftX and 0xFFFF).u16(p.leftY and 0xFFFF).u16(p.rightX and 0xFFFF).u16(p.rightY and 0xFFFF)
    }
    fun secureAttention() = encode(MsgType.SECURE_ATTENTION)
    fun disconnect() = encode(MsgType.DISCONNECT)
    // [15][motivo: 0 = keyframe, 1 = pérdida][stream]
    fun requestKeyframe(reason: Int = 0) = byteArrayOf(MsgType.REQUEST_KEYFRAME.toByte(), reason.toByte(), 0)
}
