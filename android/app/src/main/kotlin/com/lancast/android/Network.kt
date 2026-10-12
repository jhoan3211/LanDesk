package com.lancast.android

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress

// Los PC están en el Wi-Fi. Si ese Wi-Fi no tiene internet, Android manda el tráfico por los datos móviles y no
// llega a los PC: toda la app se ata a la red Wi-Fi mientras exista (las actualizaciones usan la red por defecto).
object WifiBinding {
    @Volatile private var bound: Network? = null
    private var callback: ConnectivityManager.NetworkCallback? = null

    fun start(context: Context) {
        if (callback != null) return
        val cm = context.applicationContext.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                bound = network
                cm.bindProcessToNetwork(network)
            }

            override fun onLost(network: Network) {
                if (bound == network) {
                    bound = null
                    cm.bindProcessToNetwork(null)
                }
            }
        }
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)  // También un Wi-Fi sin internet.
            .build()
        try {
            cm.registerNetworkCallback(request, cb)
            callback = cb
        } catch (_: Exception) {
        }
    }

    // Red para internet (GitHub): la predeterminada del sistema, aunque la app esté atada al Wi-Fi.
    fun internet(context: Context): Network? =
        (context.applicationContext.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager).activeNetwork
}

// Wake-on-LAN: "paquete mágico" (6 × 0xFF + 16 × la MAC) por broadcast a los puertos 9 y 7.
object WakeOnLan {
    fun parseMac(text: String): ByteArray? {
        val parts = text.split(':', '-').filter { it.isNotEmpty() }
        if (parts.size != 6) return null
        return try {
            ByteArray(6) { parts[it].toInt(16).toByte() }
        } catch (_: NumberFormatException) {
            null
        }
    }

    fun format(mac: ByteArray) = mac.joinToString(":") { "%02X".format(it.toInt() and 0xFF) }

    fun send(mac: ByteArray, lastAddress: String?) {
        val packet = ByteArray(6 + 16 * 6)
        for (i in 0 until 6) packet[i] = 0xFF.toByte()
        for (r in 0 until 16) System.arraycopy(mac, 0, packet, 6 + r * 6, 6)
        val targets = DeviceInfo.broadcastAddresses().toMutableList()
        // El PC dormido no responde ARP, pero algunos routers aún lo tienen en su tabla: también directo.
        lastAddress?.let { runCatching { targets += InetAddress.getByName(it) } }
        DatagramSocket().use { sock ->
            sock.broadcast = true
            repeat(3) {
                for (addr in targets) for (port in intArrayOf(9, 7)) {
                    runCatching { sock.send(DatagramPacket(packet, packet.size, addr, port)) }
                }
                Thread.sleep(100)
            }
        }
    }
}
