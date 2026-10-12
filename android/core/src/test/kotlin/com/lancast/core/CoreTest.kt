package com.lancast.core

import kotlin.random.Random
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

class CoreTest {
    @Test
    fun bytesAreLittleEndian() {
        val w = ByteWriter().u8(1).u16(0x0203).u32(0x04050607).u64(0x08090A0B0C0D0E0FL).str("ñ").f32(1.5f)
        val b = w.toByteArray()
        assertEquals(listOf(1, 3, 2, 7, 6, 5, 4), b.take(7).map { it.toInt() })
        val r = ByteReader(b)
        assertEquals(1, r.u8())
        assertEquals(0x0203, r.u16())
        assertEquals(0x04050607L, r.u32())
        assertEquals(0x08090A0B0C0D0E0FL, r.u64())
        assertEquals("ñ", r.str())
        assertEquals(1.5f, r.f32())
        assertTrue(r.ok)
        r.u8()
        assertFalse(r.ok, "Leer de más marca error")
    }

    @Test
    fun hkdfRfc5869Case1() {
        val ikm = ByteArray(22) { 0x0b }
        val salt = "000102030405060708090a0b0c".hexToBytes()!!
        val info = "f0f1f2f3f4f5f6f7f8f9".hexToBytes()!!
        val okm = Crypto.hkdf(ikm, salt, info, 42)
        assertEquals("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", okm.toHex())
    }

    @Test
    fun pbkdf2MatchesJca() {
        val salt = Crypto.randomBytes(32)
        val ours = Crypto.pbkdf2("123456", salt, 1000)
        val spec = javax.crypto.spec.PBEKeySpec("123456".toCharArray(), salt, 1000, 256)
        val jca = javax.crypto.SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).encoded
        assertContentEquals(jca, ours)
    }

    @Test
    fun ecdhAgreesAndAesGcmAuthenticates() {
        val a = Ecdh()
        val b = Ecdh()
        val za = a.derive(b.publicKey)
        val zb = b.derive(a.publicKey)
        assertNotNull(za)
        assertContentEquals(za, zb)
        assertEquals(32, za.size)

        val gcm = AesGcm(Crypto.randomBytes(32))
        val nonce = Session.counterNonce(7)
        val sealed = gcm.encrypt(nonce, byteArrayOf(9), "hola".toByteArray())
        assertEquals("hola", String(gcm.decrypt(nonce, byteArrayOf(9), sealed)!!))
        sealed[0] = (sealed[0].toInt() xor 1).toByte()
        assertNull(gcm.decrypt(nonce, byteArrayOf(9), sealed), "Un byte alterado se rechaza")
    }

    @Test
    fun reedSolomonRecoversAnyKShards() {
        val rnd = Random(42)
        val k = 10
        val m = 4
        val size = 64
        val block = ByteArray((k + m) * size)
        rnd.nextBytes(block, 0, k * size)
        val original = block.copyOf(k * size)
        ReedSolomon(k, m).encode(block, size)
        repeat(50) {
            val damaged = block.copyOf()
            val present = BooleanArray(k + m) { true }
            (0 until k + m).shuffled(rnd).take(m).forEach { i ->
                present[i] = false
                java.util.Arrays.fill(damaged, i * size, (i + 1) * size, 0)
            }
            assertTrue(ReedSolomon(k, m).reconstruct(damaged, present, size))
            assertContentEquals(original, damaged.copyOf(k * size))
        }
    }

    // Paquetiza un cuadro como el host (packetize_frame) para probar el reensamblado.
    private fun packetize(frame: ByteArray, frameId: Long, fec: Int, key: Boolean, epoch: Int, slice: Int = -1, last: Boolean = true): List<ByteArray> {
        val ss = MediaConst.SHARD_SIZE
        val total = (frame.size + ss - 1) / ss
        val blocks = (total + 99) / 100
        val perBlock = (total + blocks - 1) / blocks
        val out = ArrayList<ByteArray>()
        for (b in 0 until blocks) {
            val first = b * perBlock
            val k = minOf(perBlock, total - first)
            val m = if (fec <= 0) 0 else maxOf((k * fec + 99) / 100, 1)
            val buf = ByteArray((k + m) * ss)
            val off = first * ss
            System.arraycopy(frame, off, buf, 0, minOf(k * ss, frame.size - off))
            if (m > 0) ReedSolomon(k, m).encode(buf, ss)
            for (s in 0 until k + m) {
                val flags = (if (key) MediaConst.FLAG_KEYFRAME else 0) or (if (slice >= 0) MediaConst.FLAG_SLICE else 0)
                val reserved = if (slice >= 0) slice or (if (last) MediaConst.SLICE_LAST else 0) else 0
                val w = ByteWriter().u32(frameId).u16(b).u16(blocks).u8(s).u8(k).u8(m).u8(flags).u32(frame.size).u64(123).u32(45)
                    .u16(epoch).u16(reserved)
                w.raw(buf, s * ss, ss)
                out += w.toByteArray()
            }
        }
        return out
    }

    private fun feed(a: FrameAssembler, p: ByteArray): AssembledFrame? {
        val r = ByteReader(p)
        val h = VideoShardHeader.read(r)!!
        return a.add(h, p, r.position, p.size - r.position)
    }

    @Test
    fun assemblerRecoversLossWithFec() {
        val rnd = Random(7)
        val frame = ByteArray(200_000).also { rnd.nextBytes(it) }
        val packets = packetize(frame, 1, 20, true, 3).shuffled(rnd)
        val a = FrameAssembler()
        var done: AssembledFrame? = null
        packets.forEachIndexed { i, p -> if (i % 9 != 0) feed(a, p)?.let { done = it } }  // ~11 % perdido
        assertNotNull(done)
        assertContentEquals(frame, done!!.data)
        assertTrue(done!!.info.keyframe)
        assertTrue(done!!.recoveredByFec)
    }

    @Test
    fun slicesAreJoinedInOrder() {
        val a = FrameAssembler()
        val j = SliceJoiner()
        val parts = listOf(ByteArray(3000) { 1 }, ByteArray(5000) { 2 }, ByteArray(100) { 3 })
        var out: AssembledFrame? = null
        parts.forEachIndexed { i, part ->
            for (p in packetize(part, (10 + i).toLong(), 0, i == 0, 1, slice = i, last = i == parts.lastIndex)) {
                feed(a, p)?.let { unit -> j.add(unit)?.let { out = it } }
            }
        }
        assertNotNull(out)
        assertContentEquals(parts[0] + parts[1] + parts[2], out!!.data)
        assertTrue(out!!.info.keyframe)
        assertEquals(0, j.takeLost())
    }

    @Test
    fun streamConfigHasHostLayout() {
        // u32×4, codec, auto, bitrate, rc, 444, hdr, audio, fec, audio_rate, canales, ll, ir, slices, índice, activo, adaptive.
        assertEquals(1 + 38, StreamConfig().encode().size)  // = StreamConfig::write del host
        assertEquals(MsgType.STREAM_CONFIG, StreamConfig().encode()[0].toInt())
    }
}
