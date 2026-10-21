package com.lancast.core

// Datagramas de medios y reensamblado de video con FEC (port de lancast-core/media.cpp y fec.cpp).
//
// Datagrama: [cabecera 12 B: tipo u8, 3 ceros, secuencia u64 — también es el nonce y el AAD de AES-GCM]
//            [carga cifrada][tag 16 B]
// Carga de video: [VideoShardHeader 32 B][fragmento de SHARD_SIZE bytes]

object MediaConst {
    const val MAX_DATAGRAM = 1400
    const val PACKET_HEADER_SIZE = 12
    const val TAG_SIZE = 16
    const val SHARD_HEADER_SIZE = 32
    const val SHARD_SIZE = MAX_DATAGRAM - PACKET_HEADER_SIZE - TAG_SIZE - SHARD_HEADER_SIZE
    const val TYPE_VIDEO = 1
    const val TYPE_AUDIO = 2
    const val TYPE_CLIENT_HELLO = 3
    const val FLAG_KEYFRAME = 0x01
    const val FLAG_SLICE = 0x02
    const val FLAG_STREAM1 = 0x04
    const val SLICE_LAST = 0x8000
    const val MAX_TOTAL_SHARDS = 255
}

fun packetHeader(type: Int, seq: Long): ByteArray {
    val h = ByteArray(MediaConst.PACKET_HEADER_SIZE)
    h[0] = type.toByte()
    for (i in 0 until 8) h[4 + i] = (seq ushr (8 * i)).toByte()
    return h
}

class VideoShardHeader(
    val frameId: Long,
    val blockIndex: Int,
    val blockCount: Int,
    val shardIndex: Int,
    val dataShards: Int,
    val parityShards: Int,
    val flags: Int,
    val frameSize: Int,
    val captureTimeUs: Long,
    val encodeTimeUs: Long,
    val epoch: Int,
    val reserved: Int,
) {
    companion object {
        fun read(r: ByteReader): VideoShardHeader? {
            val h = VideoShardHeader(r.u32(), r.u16(), r.u16(), r.u8(), r.u8(), r.u8(), r.u8(), r.u32().toInt(), r.u64(),
                r.u32(), r.u16(), r.u16())
            if (!r.ok || h.blockCount == 0 || h.blockIndex >= h.blockCount || h.dataShards == 0 ||
                h.dataShards + h.parityShards > MediaConst.MAX_TOTAL_SHARDS || h.shardIndex >= h.dataShards + h.parityShards ||
                h.frameSize <= 0 || h.frameSize > 64 * 1024 * 1024
            ) return null
            return h
        }
    }
}

// Reed-Solomon sistemático sobre GF(2^8) (polinomio 0x11D) con matriz de Cauchy: igual que el host.
object Gf {
    val exp = IntArray(512)
    val log = IntArray(256)

    init {
        var x = 1
        for (i in 0 until 255) {
            exp[i] = x
            log[x] = i
            x = x shl 1
            if (x and 0x100 != 0) x = x xor 0x11D
        }
        for (i in 255 until 512) exp[i] = exp[i - 255]
    }

    // Tabla de multiplicar por cada coeficiente (64 KB): el host la usa para calcular la paridad rápido.
    val mulTable: Array<ByteArray> = Array(256) { c -> ByteArray(256) { x -> if (c == 0 || x == 0) 0 else exp[log[c] + log[x]].toByte() } }

    fun mul(a: Int, b: Int): Int = if (a == 0 || b == 0) 0 else exp[log[a] + log[b]]
    fun inv(a: Int): Int = if (a == 0) 0 else exp[255 - log[a]]

    // dst[dOff..] ^= c * src[sOff..]
    fun mulAdd(dst: ByteArray, dOff: Int, src: ByteArray, sOff: Int, c: Int, size: Int) {
        if (c == 0) return
        if (c == 1) {
            for (i in 0 until size) dst[dOff + i] = (dst[dOff + i].toInt() xor src[sOff + i].toInt()).toByte()
            return
        }
        val lc = log[c]
        for (i in 0 until size) {
            val s = src[sOff + i].toInt() and 0xFF
            if (s != 0) dst[dOff + i] = (dst[dOff + i].toInt() xor exp[lc + log[s]]).toByte()
        }
    }
}

class ReedSolomon(private val k: Int, private val m: Int) {
    private val matrix = IntArray(m * k) { idx -> Gf.inv(((k + idx / k) xor (idx % k)) and 0xFF) }

    private fun coef(row: Int, col: Int) = matrix[row * k + col]

    // Genera la paridad de un bloque (k + m fragmentos contiguos en `block`).
    fun encode(block: ByteArray, shardSize: Int) {
        for (i in 0 until m) {
            val off = (k + i) * shardSize
            java.util.Arrays.fill(block, off, off + shardSize, 0)
            for (j in 0 until k) {
                val c = coef(i, j)
                if (c == 0) continue
                val t = Gf.mulTable[c]
                val src = j * shardSize
                for (x in 0 until shardSize) block[off + x] = (block[off + x].toInt() xor t[block[src + x].toInt() and 0xFF].toInt()).toByte()
            }
        }
    }

    // block: (k + m) fragmentos contiguos; present: cuáles llegaron. Reconstruye los de datos que faltan.
    fun reconstruct(block: ByteArray, present: BooleanArray, shardSize: Int): Boolean {
        val missing = (0 until k).filter { !present[it] }
        if (missing.isEmpty()) return true
        val rows = (0 until k + m).filter { present[it] }.take(k)
        if (rows.size < k) return false
        val a = IntArray(k * k)
        rows.forEachIndexed { r, s ->
            if (s < k) a[r * k + s] = 1 else for (j in 0 until k) a[r * k + j] = coef(s - k, j)
        }
        val inv = invert(a) ?: return false
        val recovered = missing.map { j ->
            val out = ByteArray(shardSize)
            for (r in 0 until k) Gf.mulAdd(out, 0, block, rows[r] * shardSize, inv[j * k + r], shardSize)
            out
        }
        missing.forEachIndexed { idx, j -> System.arraycopy(recovered[idx], 0, block, j * shardSize, shardSize) }
        return true
    }

    private fun invert(src: IntArray): IntArray? {
        val n = k
        val a = src.copyOf()
        val inv = IntArray(n * n).also { for (i in 0 until n) it[i * n + i] = 1 }
        for (col in 0 until n) {
            var pivot = col
            while (pivot < n && a[pivot * n + col] == 0) pivot++
            if (pivot == n) return null
            if (pivot != col) {
                for (j in 0 until n) {
                    a[pivot * n + j] = a[col * n + j].also { a[col * n + j] = a[pivot * n + j] }
                    inv[pivot * n + j] = inv[col * n + j].also { inv[col * n + j] = inv[pivot * n + j] }
                }
            }
            val p = Gf.inv(a[col * n + col])
            for (j in 0 until n) {
                a[col * n + j] = Gf.mul(p, a[col * n + j])
                inv[col * n + j] = Gf.mul(p, inv[col * n + j])
            }
            for (r in 0 until n) {
                if (r == col) continue
                val f = a[r * n + col]
                if (f == 0) continue
                for (j in 0 until n) {
                    a[r * n + j] = a[r * n + j] xor Gf.mul(f, a[col * n + j])
                    inv[r * n + j] = inv[r * n + j] xor Gf.mul(f, inv[col * n + j])
                }
            }
        }
        return inv
    }
}

class FrameInfo(
    val frameId: Long,
    val keyframe: Boolean,
    val captureTimeUs: Long,
    val encodeTimeUs: Long,
    val epoch: Int,
    val slice: Boolean,
    val streamIndex: Int,
    val sliceIndex: Int,
    val lastSlice: Boolean,
)

class AssembledFrame(var data: ByteArray, var info: FrameInfo, var recoveredByFec: Boolean)

private fun newer(a: Long, b: Long) = ((a - b).toInt()) > 0  // ids de 32 bits con vuelta

// Reensambla cuadros a partir de fragmentos (en cualquier orden, con duplicados y pérdidas).
class FrameAssembler {
    private class Block(val k: Int, val m: Int) {
        val buffer = ByteArray((k + m) * MediaConst.SHARD_SIZE)
        val present = BooleanArray(k + m)
        var received = 0
        var done = false
    }

    private class Frame(val header: VideoShardHeader) {
        val blocks = arrayOfNulls<Block>(header.blockCount)
        var blocksDone = 0
        var usedParity = false
    }

    private val frames = java.util.TreeMap<Long, Frame>()
    private var lastCompleted: Long? = null
    private var pendingLost = 0
    private var epoch = 0
    private var hasEpoch = false
    var framesRecovered = 0L
        private set
    var framesLost = 0L
        private set

    fun takeLost(): Int = pendingLost.also { pendingLost = 0 }

    fun reset() {
        frames.clear()
        lastCompleted = null
        hasEpoch = false
        pendingLost = 0
    }

    fun add(h: VideoShardHeader, shard: ByteArray, shardOffset: Int, shardLength: Int): AssembledFrame? {
        if (shardLength != MediaConst.SHARD_SIZE) return null
        if (!hasEpoch || h.epoch != epoch) {
            if (hasEpoch && ((h.epoch - epoch).toShort()) < 0) return null  // Paquete de una configuración vieja.
            hasEpoch = true
            epoch = h.epoch
            frames.clear()
            lastCompleted = null
        }
        lastCompleted?.let { if (!newer(h.frameId, it)) return null }
        while (frames.size >= 32 && !frames.containsKey(h.frameId)) frames.remove(frames.firstKey())

        val frame = frames.getOrPut(h.frameId) { Frame(h) }
        if (frame.header.blockCount != h.blockCount || frame.header.frameSize != h.frameSize) return null
        var block = frame.blocks[h.blockIndex]
        if (block == null) {
            block = Block(h.dataShards, h.parityShards)
            frame.blocks[h.blockIndex] = block
        } else if (block.k != h.dataShards || block.m != h.parityShards) {
            return null
        }
        if (block.done || block.present[h.shardIndex]) return null
        block.present[h.shardIndex] = true
        System.arraycopy(shard, shardOffset, block.buffer, h.shardIndex * MediaConst.SHARD_SIZE, MediaConst.SHARD_SIZE)
        block.received++
        if (block.received < block.k) return null

        if ((0 until block.k).any { !block.present[it] }) {
            if (!ReedSolomon(block.k, block.m).reconstruct(block.buffer, block.present, MediaConst.SHARD_SIZE)) return null
            frame.usedParity = true
        }
        block.done = true
        if (++frame.blocksDone < frame.blocks.size) return null

        // Cuadro completo.
        val total = frame.blocks.sumOf { it!!.k } * MediaConst.SHARD_SIZE
        if (total < h.frameSize) {
            frames.remove(h.frameId)
            return null
        }
        val data = ByteArray(h.frameSize)
        var pos = 0
        for (b in frame.blocks) {
            val n = minOf(b!!.k * MediaConst.SHARD_SIZE, data.size - pos)
            if (n <= 0) break
            System.arraycopy(b.buffer, 0, data, pos, n)
            pos += n
        }
        val fh = frame.header
        val slice = fh.flags and MediaConst.FLAG_SLICE != 0
        val info = FrameInfo(fh.frameId, fh.flags and MediaConst.FLAG_KEYFRAME != 0, fh.captureTimeUs, fh.encodeTimeUs, fh.epoch,
            slice, if (fh.flags and MediaConst.FLAG_STREAM1 != 0) 1 else 0, fh.reserved and 0xFF,
            !slice || (fh.reserved and MediaConst.SLICE_LAST) != 0)

        val lost = lastCompleted?.let { ((h.frameId - it - 1).toInt()).coerceAtLeast(0) }
            ?: frames.keys.count { newer(h.frameId, it) }
        frames.keys.removeAll { !newer(it, h.frameId) }
        lastCompleted = h.frameId
        pendingLost += lost
        framesLost += lost
        if (frame.usedParity) framesRecovered++
        return AssembledFrame(data, info, frame.usedParity)
    }
}

// Junta los slices de un cuadro y lo entrega completo cuando llega el último; si falta uno, se descarta.
class SliceJoiner {
    private var pending: AssembledFrame? = null
    private var expectedUnit = 0L
    private var expectedSlice = 0
    private var lost = 0

    fun takeLost(): Int = lost.also { lost = 0 }

    fun reset() {
        pending = null
    }

    fun add(unit: AssembledFrame): AssembledFrame? {
        pending?.let { if (it.info.epoch != unit.info.epoch) pending = null }
        if (!unit.info.slice) {
            if (pending != null) {
                lost++
                pending = null
            }
            return unit
        }
        val p = pending
        if (unit.info.sliceIndex == 0) {
            if (p != null) lost++
            pending = unit
        } else if (p == null || unit.info.frameId != expectedUnit || unit.info.sliceIndex != expectedSlice) {
            if (p != null) lost++
            pending = null
            return null
        } else {
            p.data = p.data + unit.data
            p.info = FrameInfo(unit.info.frameId, p.info.keyframe || unit.info.keyframe, p.info.captureTimeUs,
                unit.info.encodeTimeUs, p.info.epoch, true, p.info.streamIndex, p.info.sliceIndex, unit.info.lastSlice)
            p.recoveredByFec = p.recoveredByFec || unit.recoveredByFec
        }
        val cur = pending!!
        expectedUnit = cur.info.frameId + 1
        expectedSlice = unit.info.sliceIndex + 1
        if (!cur.info.lastSlice) return null
        pending = null
        cur.info = FrameInfo(cur.info.frameId, cur.info.keyframe, cur.info.captureTimeUs, cur.info.encodeTimeUs, cur.info.epoch,
            false, cur.info.streamIndex, cur.info.sliceIndex, true)
        return cur
    }
}
