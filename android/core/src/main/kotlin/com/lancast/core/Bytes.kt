package com.lancast.core

// Serialización binaria little-endian: el mismo formato que lancast-core/bytes.hpp del host.

class ByteWriter(capacity: Int = 64) {
    private var buf = ByteArray(capacity)
    var size = 0
        private set

    private fun ensure(n: Int) {
        if (size + n > buf.size) buf = buf.copyOf(maxOf(buf.size * 2, size + n))
    }

    fun u8(v: Int) = apply {
        ensure(1)
        buf[size++] = v.toByte()
    }

    fun bool(v: Boolean) = u8(if (v) 1 else 0)

    fun u16(v: Int) = apply {
        ensure(2)
        buf[size++] = v.toByte()
        buf[size++] = (v ushr 8).toByte()
    }

    fun u32(v: Int) = apply {
        ensure(4)
        for (i in 0 until 4) buf[size++] = (v ushr (8 * i)).toByte()
    }

    fun u32(v: Long) = u32(v.toInt())

    fun u64(v: Long) = apply {
        ensure(8)
        for (i in 0 until 8) buf[size++] = (v ushr (8 * i)).toByte()
    }

    fun i32(v: Int) = u32(v)

    fun f32(v: Float) = u32(java.lang.Float.floatToRawIntBits(v))

    fun raw(data: ByteArray, offset: Int = 0, length: Int = data.size - offset) = apply {
        ensure(length)
        System.arraycopy(data, offset, buf, size, length)
        size += length
    }

    fun str(s: String) = apply {
        val b = s.toByteArray(Charsets.UTF_8)
        u32(b.size)
        raw(b)
    }

    fun toByteArray(): ByteArray = buf.copyOf(size)
}

// Lector tolerante: ante datos insuficientes devuelve ceros y marca ok = false (como el del host).
class ByteReader(private val data: ByteArray, private var pos: Int = 0, private val end: Int = data.size) {
    var ok = true
        private set

    val remaining: Int get() = end - pos
    val position: Int get() = pos

    private fun need(n: Int): Boolean {
        if (!ok || remaining < n) {
            ok = false
            return false
        }
        return true
    }

    fun u8(): Int {
        if (!need(1)) return 0
        return data[pos++].toInt() and 0xFF
    }

    fun bool(): Boolean = u8() != 0

    fun u16(): Int {
        if (!need(2)) return 0
        val v = (data[pos].toInt() and 0xFF) or ((data[pos + 1].toInt() and 0xFF) shl 8)
        pos += 2
        return v
    }

    // Sin signo (0..2^32-1).
    fun u32(): Long = i32().toLong() and 0xFFFFFFFFL

    fun i32(): Int {
        if (!need(4)) return 0
        var v = 0
        for (i in 0 until 4) v = v or ((data[pos + i].toInt() and 0xFF) shl (8 * i))
        pos += 4
        return v
    }

    fun u64(): Long {
        if (!need(8)) return 0
        var v = 0L
        for (i in 0 until 8) v = v or ((data[pos + i].toLong() and 0xFF) shl (8 * i))
        pos += 8
        return v
    }

    fun f32(): Float = java.lang.Float.intBitsToFloat(i32())

    fun raw(n: Int): ByteArray {
        if (n < 0 || !need(n)) return ByteArray(0)
        val out = data.copyOfRange(pos, pos + n)
        pos += n
        return out
    }

    fun str(max: Int = 1 shl 20): String {
        val n = u32()
        if (n > max) {
            ok = false
            return ""
        }
        return String(raw(n.toInt()), Charsets.UTF_8)
    }

    fun blob(max: Int = 64 shl 20): ByteArray {
        val n = u32()
        if (n > max) {
            ok = false
            return ByteArray(0)
        }
        return raw(n.toInt())
    }
}

fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it.toInt() and 0xFF) }

fun String.hexToBytes(): ByteArray? {
    if (length % 2 != 0) return null
    return try {
        ByteArray(length / 2) { substring(it * 2, it * 2 + 2).toInt(16).toByte() }
    } catch (_: NumberFormatException) {
        null
    }
}
