package com.rconnect.android.winrm

import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec
import java.security.MessageDigest

// Small primitives NTLM needs that Android does not ship (MD4, RC4) or that are one-liners (MD5, HMAC-MD5).

fun md5(vararg parts: ByteArray): ByteArray {
    val d = MessageDigest.getInstance("MD5")
    for (p in parts) d.update(p)
    return d.digest()
}

fun hmacMd5(key: ByteArray, vararg parts: ByteArray): ByteArray {
    val mac = Mac.getInstance("HmacMD5")
    mac.init(SecretKeySpec(key, "HmacMD5"))
    for (p in parts) mac.update(p)
    return mac.doFinal()
}

/** Stateful RC4: the stream carries on from one message to the next, as NTLM sealing needs. */
class Rc4(key: ByteArray) {
    private val s = IntArray(256) { it }
    private var i = 0
    private var j = 0

    init {
        var k = 0
        for (n in 0 until 256) {
            k = (k + s[n] + (key[n % key.size].toInt() and 0xFF)) and 0xFF
            val t = s[n]; s[n] = s[k]; s[k] = t
        }
    }

    fun process(data: ByteArray): ByteArray {
        val out = ByteArray(data.size)
        for (n in data.indices) {
            i = (i + 1) and 0xFF
            j = (j + s[i]) and 0xFF
            val t = s[i]; s[i] = s[j]; s[j] = t
            out[n] = (data[n].toInt() xor s[(s[i] + s[j]) and 0xFF]).toByte()
        }
        return out
    }
}

fun md4(message: ByteArray): ByteArray {
    val padLen = ((56 - (message.size + 1) % 64) + 64) % 64
    val total = message.size + 1 + padLen + 8
    val buf = ByteArray(total)
    System.arraycopy(message, 0, buf, 0, message.size)
    buf[message.size] = 0x80.toByte()
    val bits = message.size.toLong() * 8
    for (n in 0 until 8) buf[total - 8 + n] = (bits ushr (8 * n)).toByte()

    var a = 0x67452301
    var b = 0xefcdab89L.toInt()
    var c = 0x98badcfeL.toInt()
    var d = 0x10325476
    val x = IntArray(16)

    fun rol(v: Int, s: Int) = (v shl s) or (v ushr (32 - s))
    fun f(x: Int, y: Int, z: Int) = (x and y) or (x.inv() and z)
    fun g(x: Int, y: Int, z: Int) = (x and y) or (x and z) or (y and z)
    fun h(x: Int, y: Int, z: Int) = x xor y xor z

    for (block in 0 until total step 64) {
        for (n in 0 until 16) {
            val o = block + n * 4
            x[n] = (buf[o].toInt() and 0xFF) or ((buf[o + 1].toInt() and 0xFF) shl 8) or
                ((buf[o + 2].toInt() and 0xFF) shl 16) or ((buf[o + 3].toInt() and 0xFF) shl 24)
        }
        val aa = a; val bb = b; val cc = c; val dd = d

        val s1 = intArrayOf(3, 7, 11, 19)
        for (n in 0 until 16) {
            val t = rol(a + f(b, c, d) + x[n], s1[n % 4])
            a = d; d = c; c = b; b = t
        }
        val s2 = intArrayOf(3, 5, 9, 13)
        for (n in 0 until 16) {
            val k = (n % 4) * 4 + n / 4
            val t = rol(a + g(b, c, d) + x[k] + 0x5a827999, s2[n % 4])
            a = d; d = c; c = b; b = t
        }
        val s3 = intArrayOf(3, 9, 11, 15)
        val order = intArrayOf(0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15)
        for (n in 0 until 16) {
            val t = rol(a + h(b, c, d) + x[order[n]] + 0x6ed9eba1, s3[n % 4])
            a = d; d = c; c = b; b = t
        }
        a += aa; b += bb; c += cc; d += dd
    }
    val out = ByteArray(16)
    for ((n, v) in intArrayOf(a, b, c, d).withIndex())
        for (k in 0 until 4) out[n * 4 + k] = (v ushr (8 * k)).toByte()
    return out
}
