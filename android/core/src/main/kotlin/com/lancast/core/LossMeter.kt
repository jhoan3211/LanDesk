package com.lancast.core

// Pérdida de paquetes por la secuencia de los datagramas, tolerante al desorden (igual que el cliente de PC). El
// host numera cada paquete al prepararlo y envía el video en tandas: un paquete de audio puede llegar antes que video
// con número menor. Un número se da por perdido solo cuando ya llegaron `grace` paquetes posteriores sin él.
class LossMeter(private val grace: Long = 64) {
    private val seen = LongArray(SLOTS) { -1L }
    private var started = false
    private var high = 0L
    private var next = 0L
    private var expected = 0L
    private var lost = 0L

    fun received(seq: Long) {
        if (!started || seq + RESTART < high) {  // Primer paquete o el host reinició la secuencia.
            started = true
            high = seq
            next = seq
            seen.fill(-1L)
        }
        if (seq < next) return  // Llegó después del margen: ya se contó como perdido.
        seen[(seq % SLOTS).toInt()] = seq
        if (seq > high) high = seq
        while (high - next > grace) {
            expected++
            if (seen[(next % SLOTS).toInt()] != next) lost++
            next++
        }
    }

    // Porcentaje perdido desde la llamada anterior.
    fun takePercent(): Double {
        val p = if (expected == 0L) 0.0 else lost * 100.0 / expected
        expected = 0
        lost = 0
        return p
    }

    private companion object {
        const val SLOTS = 4096
        const val RESTART = 100_000L
    }
}
