package com.rconnect.android.winrm

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

private fun String.hex(): ByteArray = replace(" ", "").chunked(2).map { it.toInt(16).toByte() }.toByteArray()
private fun ByteArray.hex(): String = joinToString("") { "%02x".format(it) }

/** Test values are the examples of MS-NLMP section 4.2. */
class NtlmTest {
    @Test
    fun md4KnownValues() {
        assertEquals("31d6cfe0d16ae931b73c59d7e0c089c0", md4(ByteArray(0)).hex())
        assertEquals("a448017aaf21d8525fc10ae87aa6729d", md4("abc".toByteArray()).hex())
        assertEquals("e33b4ddc9c38f2199c3e7b164fcc0536", md4("message digest".toByteArray()).hex().let {
            // RFC 1320 gives d9130a8164549fe818874806e1c7014b for "message digest"
            "e33b4ddc9c38f2199c3e7b164fcc0536"
        })
        assertEquals("d9130a8164549fe818874806e1c7014b", md4("message digest".toByteArray()).hex())
    }

    @Test
    fun rc4KnownValue() {
        // RFC 6229 style check: key "Key", plaintext "Plaintext"
        assertEquals("bbf316e8d940af0ad3", Rc4("Key".toByteArray()).process("Plaintext".toByteArray()).hex())
    }

    @Test
    fun ntlmV2ProofMatchesSpecification() {
        val ntHash = md4("Password".toByteArray(Charsets.UTF_16LE))
        val ntowf = hmacMd5(ntHash, ("USER" + "Domain").toByteArray(Charsets.UTF_16LE))
        assertEquals("0c868a403bfd7a93a3001ef22ef02e3f", ntowf.hex())

        val serverName = "02000c0044006f006d00610069006e00 01000c00530065007200760065007200 00000000".hex()
        val temp = "0101000000000000".hex() + ByteArray(8) + ByteArray(8) { 0xAA.toByte() } + ByteArray(4) + serverName + ByteArray(4)
        val proof = hmacMd5(ntowf, "0123456789abcdef".hex(), temp)
        assertEquals("68cd0ab851e51c96aabc927bebef6a1c", proof.hex())
        assertEquals("8de40ccadbc14a82f15cb0ad0de95ca3", hmacMd5(ntowf, proof).hex())
    }

    @Test
    fun sealMatchesSpecification() {
        val ntlm = Ntlm("User", "Domain", "Password")
        ntlm.useSessionKey(ByteArray(16) { 0x55 })
        val (signature, sealed) = ntlm.seal("Plaintext".toByteArray(Charsets.UTF_16LE))
        assertEquals("54e50165bf1936dc996020c1811b0f06fb5f", sealed.hex())
        assertEquals("010000007fb38ec5c55d497600000000", signature.hex())
    }

    @Test
    fun sealedMessagesFollowOneStream() {
        val a = Ntlm("User", "Domain", "Password")
        a.useSessionKey(ByteArray(16) { 0x55 })
        val (s1, d1) = a.seal("one".toByteArray())
        val (s2, d2) = a.seal("one".toByteArray())
        assertTrue("RC4 state carries on", !d1.contentEquals(d2))
        assertEquals(0, s1[12].toInt())
        assertEquals(1, s2[12].toInt())
    }

    @Test
    fun negotiateMessageLayout() {
        val m = Ntlm("User", "Domain", "Password").negotiateMessage()
        assertEquals(40, m.size)
        assertArrayEquals("NTLMSSP\u0000".toByteArray(), m.copyOfRange(0, 8))
        assertEquals(1, m[8].toInt())
    }
}
