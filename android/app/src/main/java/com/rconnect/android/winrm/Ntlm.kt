package com.rconnect.android.winrm

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.SecureRandom

/**
 * NTLMv2 with message sealing, as WinRM over HTTP uses it (MS-NLMP). Sign-in is Negotiate -> Challenge ->
 * Authenticate; afterwards [seal] and [unseal] encrypt each SOAP message.
 *
 * [random] and [nowFileTime] are parameters so tests can reproduce the specification's examples.
 */
class Ntlm(
    user: String,
    domain: String,
    private val password: String,
    private val random: (Int) -> ByteArray = { n -> ByteArray(n).also { SecureRandom().nextBytes(it) } },
    private val nowFileTime: () -> Long = { (System.currentTimeMillis() + 11644473600000L) * 10000L },
) {
    private val user: String
    private val domain: String

    init {
        // DOMAIN\user in the user field
        val slash = user.indexOf('\\')
        if (slash >= 0) {
            this.domain = user.substring(0, slash)
            this.user = user.substring(slash + 1)
        } else {
            this.domain = domain
            this.user = user
        }
    }

    private var negotiate = ByteArray(0)
    private var clientSign = ByteArray(0)
    private var serverSign = ByteArray(0)
    private var clientSeal: Rc4? = null
    private var serverSeal: Rc4? = null
    private var sendSeq = 0
    private var receiveSeq = 0

    fun negotiateMessage(): ByteArray {
        val b = ByteBuffer.allocate(40).order(ByteOrder.LITTLE_ENDIAN)
        b.put(SIGNATURE).putInt(1).putInt(OUR_FLAGS)
        b.position(32) // empty domain and workstation fields
        b.put(byteArrayOf(10, 0, 0x61, 0x4A, 0, 0, 0, 15)) // version 10.0 build 19041, NTLM revision 15
        negotiate = b.array()
        return negotiate
    }

    fun authenticateMessage(challenge: ByteArray): ByteArray {
        val c = ByteBuffer.wrap(challenge).order(ByteOrder.LITTLE_ENDIAN)
        require(challenge.size >= 48 && challenge.copyOfRange(0, 8).contentEquals(SIGNATURE) && c.getInt(8) == 2) {
            "Not an NTLM challenge"
        }
        val serverFlags = c.getInt(20)
        val serverChallenge = challenge.copyOfRange(24, 32)
        val tiLen = c.getShort(40).toInt() and 0xFFFF
        val tiOff = c.getInt(44)
        val targetInfo = challenge.copyOfRange(tiOff, tiOff + tiLen)
        val needed = NEG_SEAL or NEG_SIGN or NEG_ESS or NEG_UNICODE or NEG_KEY_EXCH
        require(serverFlags and needed == needed) { "The server does not support NTLM message encryption" }
        val flags = serverFlags and (NEG_UNICODE or NEG_REQUEST_TARGET or NEG_SIGN or NEG_SEAL or NEG_NTLM or
            NEG_ALWAYS_SIGN or NEG_ESS or NEG_TARGET_INFO or NEG_VERSION or NEG_128 or NEG_KEY_EXCH or NEG_56)

        // The response proves the password without sending it.
        val ntHash = md4(password.toByteArray(Charsets.UTF_16LE))
        val ntowf = hmacMd5(ntHash, (user.uppercase() + domain).toByteArray(Charsets.UTF_16LE))
        val clientChallenge = random(8)
        val (info, serverTime) = withMicFlag(targetInfo)
        val time = serverTime ?: nowFileTime()
        val temp = ByteArrayOutputStream().apply {
            write(byteArrayOf(1, 1, 0, 0, 0, 0, 0, 0))
            write(le64(time))
            write(clientChallenge)
            write(ByteArray(4))
            write(info)
            write(ByteArray(4))
        }.toByteArray()
        val ntProof = hmacMd5(ntowf, serverChallenge, temp)
        val ntResponse = ntProof + temp
        val lmResponse = if (serverTime != null) ByteArray(24)
        else hmacMd5(ntowf, serverChallenge, clientChallenge) + clientChallenge

        val keyExchangeKey = hmacMd5(ntowf, ntProof)
        val sessionKey = random(16)
        val encryptedKey = Rc4(keyExchangeKey).process(sessionKey)
        deriveKeys(sessionKey)

        val domainBytes = domain.toByteArray(Charsets.UTF_16LE)
        val userBytes = user.toByteArray(Charsets.UTF_16LE)
        val header = 88
        val fields = listOf(lmResponse, ntResponse, domainBytes, userBytes, ByteArray(0), encryptedKey)
        val offsets = IntArray(fields.size)
        var off = header
        for ((n, f) in fields.withIndex()) { offsets[n] = off; off += f.size }

        val out = ByteBuffer.allocate(off).order(ByteOrder.LITTLE_ENDIAN)
        out.put(SIGNATURE).putInt(3)
        // field order in the message: LM, NT, domain, user, workstation, session key
        for (n in fields.indices) {
            out.putShort(fields[n].size.toShort()).putShort(fields[n].size.toShort()).putInt(offsets[n])
        }
        out.putInt(flags)
        out.put(byteArrayOf(10, 0, 0x61, 0x4A, 0, 0, 0, 15))
        val micAt = out.position()
        out.put(ByteArray(16))
        for (f in fields) out.put(f)
        val message = out.array()
        if (serverTime != null) {
            val mic = hmacMd5(sessionKey, negotiate, challenge, message)
            System.arraycopy(mic, 0, message, micAt, 16)
        }
        return message
    }

    /** Signature (16 bytes) and sealed data of one outgoing message. */
    fun seal(plain: ByteArray): Pair<ByteArray, ByteArray> {
        val seq = le32(sendSeq)
        val sealed = clientSeal!!.process(plain)
        val checksum = clientSeal!!.process(hmacMd5(clientSign, seq, plain).copyOf(8))
        sendSeq++
        return Pair(le32(1) + checksum + seq, sealed)
    }

    /** Decrypts one incoming message; false in the second part if its signature does not match. */
    fun unseal(signature: ByteArray, sealed: ByteArray): Pair<ByteArray, Boolean> {
        val seq = le32(receiveSeq)
        val plain = serverSeal!!.process(sealed)
        val checksum = serverSeal!!.process(signature.copyOfRange(4, 12))
        val expected = hmacMd5(serverSign, seq, plain).copyOf(8)
        receiveSeq++
        return Pair(plain, checksum.contentEquals(expected))
    }

    // Adds "this message has a MIC" to the server's target info; returns the server's timestamp if it sent one.
    private fun withMicFlag(info: ByteArray): Pair<ByteArray, Long?> {
        val b = ByteBuffer.wrap(info).order(ByteOrder.LITTLE_ENDIAN)
        val out = ByteArrayOutputStream()
        var timestamp: Long? = null
        var flagsDone = false
        while (b.remaining() >= 4) {
            val id = b.getShort().toInt() and 0xFFFF
            val len = b.getShort().toInt() and 0xFFFF
            if (id == AV_EOL) break
            val value = ByteArray(len).also { b.get(it) }
            if (id == AV_TIMESTAMP && len == 8) timestamp = ByteBuffer.wrap(value).order(ByteOrder.LITTLE_ENDIAN).getLong()
            if (id == AV_FLAGS && len == 4) {
                val f = ByteBuffer.wrap(value).order(ByteOrder.LITTLE_ENDIAN).getInt() or 2
                out.write(avPair(AV_FLAGS, le32(f)))
                flagsDone = true
            } else {
                out.write(avPair(id, value))
            }
        }
        if (timestamp != null && !flagsDone) out.write(avPair(AV_FLAGS, le32(2)))
        out.write(avPair(AV_EOL, ByteArray(0)))
        return Pair(out.toByteArray(), timestamp)
    }

    private fun deriveKeys(sessionKey: ByteArray) {
        fun magic(s: String) = (s + "\u0000").toByteArray(Charsets.US_ASCII)
        clientSign = md5(sessionKey, magic("session key to client-to-server signing key magic constant"))
        serverSign = md5(sessionKey, magic("session key to server-to-client signing key magic constant"))
        clientSeal = Rc4(md5(sessionKey, magic("session key to client-to-server sealing key magic constant")))
        serverSeal = Rc4(md5(sessionKey, magic("session key to server-to-client sealing key magic constant")))
        sendSeq = 0
        receiveSeq = 0
    }

    /** For tests: the keys of a given session key. */
    internal fun useSessionKey(sessionKey: ByteArray) = deriveKeys(sessionKey)

    companion object {
        private val SIGNATURE = "NTLMSSP\u0000".toByteArray(Charsets.US_ASCII)
        const val NEG_UNICODE = 0x1
        const val NEG_REQUEST_TARGET = 0x4
        const val NEG_SIGN = 0x10
        const val NEG_SEAL = 0x20
        const val NEG_NTLM = 0x200
        const val NEG_ALWAYS_SIGN = 0x8000
        const val NEG_ESS = 0x80000
        const val NEG_TARGET_INFO = 0x800000
        const val NEG_VERSION = 0x2000000
        const val NEG_128 = 0x20000000
        const val NEG_KEY_EXCH = 0x40000000
        const val NEG_56 = 0x80000000.toInt()
        private const val OUR_FLAGS = NEG_UNICODE or NEG_REQUEST_TARGET or NEG_SIGN or NEG_SEAL or NEG_NTLM or
            NEG_ALWAYS_SIGN or NEG_ESS or NEG_VERSION or NEG_128 or NEG_KEY_EXCH or NEG_56
        private const val AV_EOL = 0
        private const val AV_FLAGS = 6
        private const val AV_TIMESTAMP = 7

        fun le32(v: Int): ByteArray = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(v).array()
        fun le64(v: Long): ByteArray = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(v).array()
        private fun avPair(id: Int, value: ByteArray): ByteArray =
            ByteBuffer.allocate(4 + value.size).order(ByteOrder.LITTLE_ENDIAN)
                .putShort(id.toShort()).putShort(value.size.toShort()).put(value).array()
    }
}
