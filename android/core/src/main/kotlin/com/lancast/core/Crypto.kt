package com.lancast.core

import java.math.BigInteger
import java.security.AlgorithmParameters
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

// Criptografía compatible con el host (lancast-platform/crypto.cpp y session_crypto.cpp):
// ECDH P-256 (clave pública X || Y de 32 bytes cada una, big-endian), AES-256-GCM (tag de 16 bytes),
// HMAC-SHA256, HKDF (RFC 5869) y PBKDF2-HMAC-SHA256.
object Crypto {
    val random = SecureRandom()

    fun randomBytes(n: Int) = ByteArray(n).also { random.nextBytes(it) }

    fun sha256(vararg parts: ByteArray): ByteArray {
        val md = MessageDigest.getInstance("SHA-256")
        parts.forEach { md.update(it) }
        return md.digest()
    }

    fun hmac(key: ByteArray, vararg parts: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        parts.forEach { mac.update(it) }
        return mac.doFinal()
    }

    fun hkdf(ikm: ByteArray, salt: ByteArray, info: String, length: Int) = hkdf(ikm, salt, info.toByteArray(Charsets.UTF_8), length)

    fun hkdf(ikm: ByteArray, salt: ByteArray, info: ByteArray, length: Int): ByteArray {
        val prk = hmac(if (salt.isEmpty()) ByteArray(32) else salt, ikm)
        val out = java.io.ByteArrayOutputStream()
        var t = ByteArray(0)
        var counter = 1
        while (out.size() < length) {
            t = hmac(prk, t, info, byteArrayOf(counter.toByte()))
            out.write(t)
            counter++
        }
        return out.toByteArray().copyOf(length)
    }

    // PBKDF2-HMAC-SHA256 propio (32 bytes): igual en cualquier versión de Android.
    fun pbkdf2(password: String, salt: ByteArray, iterations: Int): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(password.toByteArray(Charsets.UTF_8), "HmacSHA256"))
        mac.update(salt)
        mac.update(byteArrayOf(0, 0, 0, 1))
        var u = mac.doFinal()
        val result = u.copyOf()
        for (i in 1 until iterations) {
            u = mac.doFinal(u)
            for (j in result.indices) result[j] = (result[j].toInt() xor u[j].toInt()).toByte()
        }
        return result
    }

    fun equalsConstantTime(a: ByteArray, b: ByteArray) = MessageDigest.isEqual(a, b)
}

// ECDH efímero sobre P-256.
class Ecdh {
    private val keyPair = KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1"), Crypto.random) }
        .generateKeyPair()

    val publicKey: ByteArray = (keyPair.public as ECPublicKey).w.let { fixed32(it.affineX) + fixed32(it.affineY) }

    // Secreto compartido: coordenada X (32 bytes, big-endian), como el host.
    fun derive(peer: ByteArray): ByteArray? {
        if (peer.size != 64) return null
        return try {
            val params = AlgorithmParameters.getInstance("EC").apply { init(ECGenParameterSpec("secp256r1")) }
                .getParameterSpec(ECParameterSpec::class.java)
            val point = ECPoint(BigInteger(1, peer.copyOfRange(0, 32)), BigInteger(1, peer.copyOfRange(32, 64)))
            val pub = KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(point, params))
            KeyAgreement.getInstance("ECDH").run {
                init(keyPair.private)
                doPhase(pub, true)
                generateSecret()
            }.let { if (it.size == 32) it else fixed32(BigInteger(1, it)) }
        } catch (_: Exception) {
            null
        }
    }

    private fun fixed32(v: BigInteger): ByteArray {
        val b = v.toByteArray()
        return when {
            b.size == 32 -> b
            b.size > 32 -> b.copyOfRange(b.size - 32, b.size)
            else -> ByteArray(32 - b.size) + b
        }
    }
}

class AesGcm(key: ByteArray) {
    private val key = SecretKeySpec(key, "AES")
    private val cipher: Cipher = Cipher.getInstance("AES/GCM/NoPadding")

    fun encrypt(nonce: ByteArray, aad: ByteArray?, plain: ByteArray, offset: Int = 0, length: Int = plain.size): ByteArray {
        cipher.init(Cipher.ENCRYPT_MODE, key, GCMParameterSpec(128, nonce))
        if (aad != null) cipher.updateAAD(aad)
        return cipher.doFinal(plain, offset, length)
    }

    // null si la autenticación falla.
    fun decrypt(nonce: ByteArray, aad: ByteArray?, sealed: ByteArray, offset: Int = 0, length: Int = sealed.size - offset): ByteArray? =
        try {
            cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, nonce))
            if (aad != null) cipher.updateAAD(aad)
            cipher.doFinal(sealed, offset, length)
        } catch (_: Exception) {
            null
        }
}

// Derivación de claves del handshake (ver session_crypto.hpp del host).
object SessionCrypto {
    const val PIN_ITERATIONS = 100_000

    class Keys(
        val controlC2h: ByteArray,
        val controlH2c: ByteArray,
        val mediaC2h: ByteArray,
        val mediaH2c: ByteArray,
        val fileC2h: ByteArray,
        val fileH2c: ByteArray,
        val pairingSecret: ByteArray,
    )

    fun transcript(hello: ByteArray, helloAck: ByteArray) =
        Crypto.sha256("lancast-v1-transcript".toByteArray(Charsets.UTF_8), hello, helloAck)

    fun authKeyFromPin(pin: String, th: ByteArray) = Crypto.pbkdf2(pin, th, PIN_ITERATIONS)

    fun authKeyFromSecret(secret: ByteArray, th: ByteArray) = Crypto.hkdf(secret, th, "lancast auth", 32)

    fun proof(authKey: ByteArray, role: String, th: ByteArray, z: ByteArray) =
        Crypto.hmac(authKey, role.toByteArray(Charsets.UTF_8), th, z)

    fun deriveKeys(z: ByteArray, authKey: ByteArray, th: ByteArray): Keys {
        val master = Crypto.hkdf(z + authKey, th, "lancast session", 32)
        fun k(info: String) = Crypto.hkdf(master, ByteArray(0), info, 32)
        return Keys(k("control c2h"), k("control h2c"), k("media c2h"), k("media h2c"), k("file c2h"), k("file h2c"),
            k("pairing secret"))
    }
}
