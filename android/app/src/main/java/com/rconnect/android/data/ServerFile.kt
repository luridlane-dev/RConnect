package com.rconnect.android.data

import org.json.JSONArray
import org.json.JSONObject
import java.security.SecureRandom
import java.util.Base64
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.PBEKeySpec
import javax.crypto.spec.SecretKeySpec

/**
 * The server list file ("rdpmanager-export", version 1): the same file the Windows program imports and exports.
 * With a passphrase it is encrypted (PBKDF2-SHA256, AES-256-GCM) and carries the saved passwords; without one it
 * holds no passwords.
 */
object ServerFile {
    sealed class Read {
        /** [unusable]: entries the phone cannot use (no address or no account name). */
        data class Ok(val servers: List<ServerEntry>, val unusable: Int = 0) : Read()
        data object NeedPassphrase : Read()
        data object WrongPassphrase : Read()
        data object Invalid : Read()
    }

    private const val FORMAT = "rdpmanager-export"
    private const val ITERATIONS = 210_000
    private const val NONCE = 12
    private const val TAG = 16

    fun read(text: String, passphrase: String = ""): Read {
        val root = try {
            // Editors on Windows put a byte order mark in front; it is not whitespace, so trim() keeps it.
            val trimmed = text.trimStart(Char(0xFEFF)).trim()
            if (trimmed.startsWith("[")) JSONObject().put("Content", JSONObject().put("Connections", JSONArray(trimmed)))
            else JSONObject(trimmed)
        } catch (e: Exception) {
            return Read.Invalid
        }
        if (root.has("Format") && root.optString("Format") != FORMAT) return Read.Invalid
        val bundle = if (root.optBoolean("Encrypted")) {
            if (passphrase.isEmpty()) return Read.NeedPassphrase
            val plain = open(root, passphrase) ?: return Read.WrongPassphrase
            try { JSONObject(String(plain, Charsets.UTF_8)) } catch (e: Exception) { return Read.Invalid }
        } else {
            root.optJSONObject("Content") ?: return Read.Invalid
        }
        val connections = bundle.optJSONArray("Connections") ?: return Read.Invalid
        val secrets = bundle.optJSONObject("Secrets") ?: JSONObject()
        val servers = (0 until connections.length()).mapNotNull { toEntry(connections.optJSONObject(it) ?: return@mapNotNull null, secrets) }
        return Read.Ok(servers, connections.length() - servers.size)
    }

    fun write(servers: List<ServerEntry>, passphrase: String): String {
        val connections = JSONArray()
        val secrets = JSONObject()
        for (s in servers) {
            val (domain, user) = if (s.user.contains('\\')) s.user.substringBefore('\\') to s.user.substringAfter('\\') else "" to s.user
            // What the file had that the phone does not use goes back in; the phone's own fields win.
            val c = runCatching { JSONObject(s.extra) }.getOrDefault(JSONObject())
            c.put("Id", s.id).put("Name", s.title).put("Address", s.host).put("Port", s.port)
                .put("UserName", user).put("Domain", domain)
            if (s.group.isNotEmpty()) c.put("Group", s.group) else c.remove("Group")
            connections.put(c)
            if (passphrase.isNotEmpty() && s.password.isNotEmpty()) secrets.put(s.id, s.password)
        }
        val bundle = JSONObject().put("Connections", connections)
        val root = JSONObject().put("Format", FORMAT).put("Version", 1)
        if (passphrase.isEmpty()) return root.put("Encrypted", false).put("Content", bundle).toString(2)

        bundle.put("Secrets", secrets)
        val random = SecureRandom()
        val salt = ByteArray(16).also(random::nextBytes)
        val nonce = ByteArray(NONCE).also(random::nextBytes)
        val key = derive(passphrase, salt, ITERATIONS, "PBKDF2WithHmacSHA256")
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG * 8, nonce))
        val sealed = cipher.doFinal(bundle.toString().toByteArray(Charsets.UTF_8)) // ciphertext | tag
        val cut = sealed.size - TAG
        // The file layout is nonce | tag | ciphertext.
        val payload = nonce + sealed.copyOfRange(cut, sealed.size) + sealed.copyOfRange(0, cut)
        return root.put("Encrypted", true).put("Iterations", ITERATIONS)
            .put("Salt", Base64.getEncoder().encodeToString(salt))
            .put("Payload", Base64.getEncoder().encodeToString(payload)).toString(2)
    }

    private fun toEntry(c: JSONObject, secrets: JSONObject): ServerEntry? {
        val host = c.optString("Address").trim()
        val user = c.optString("UserName").trim()
        if (host.isEmpty() || user.isEmpty()) return null
        val domain = c.optString("Domain").trim()
        val extra = JSONObject(c.toString())
        for (known in OWN_FIELDS) extra.remove(known)
        return ServerEntry(
            id = UUID.randomUUID().toString(),
            name = c.optString("Name").trim().takeIf { it != host }.orEmpty(),
            host = host,
            user = if (domain.isNotEmpty()) "$domain\\$user" else user,
            password = secrets.optString(c.optString("Id")),
            group = c.optString("Group").trim(),
            port = c.optInt("Port", 3389).takeIf { it in 1..65535 } ?: 3389,
            extra = if (extra.length() == 0) "" else extra.toString(),
        )
    }

    // The fields the phone keeps itself; everything else in an entry is carried in ServerEntry.extra.
    private val OWN_FIELDS = listOf("Id", "Name", "Address", "Port", "UserName", "Domain", "Group")

    private fun derive(passphrase: String, salt: ByteArray, iterations: Int, algorithm: String): ByteArray =
        SecretKeyFactory.getInstance(algorithm).generateSecret(PBEKeySpec(passphrase.toCharArray(), salt, iterations, 256)).encoded

    /** The decrypted bundle, or null when no combination authenticates (a wrong passphrase). */
    private fun open(root: JSONObject, passphrase: String): ByteArray? {
        val salt = decode(root.optString("Salt"))
        val payload = decode(root.optString("Payload"))
        val iterations = root.optInt("Iterations", ITERATIONS).takeIf { it > 0 } ?: ITERATIONS
        if (salt.isEmpty() || payload.size < NONCE + TAG) return null
        val nonce = payload.copyOfRange(0, NONCE)
        // The usual variants of the old program's settings: the passphrase as typed or trimmed, PBKDF2 with
        // SHA-256, SHA-1 or SHA-512, and the tag before or after the ciphertext.
        for (pass in listOf(passphrase, passphrase.trim()).distinct()) {
            for (algorithm in listOf("PBKDF2WithHmacSHA256", "PBKDF2WithHmacSHA1", "PBKDF2WithHmacSHA512")) {
                val key = try { derive(pass, salt, iterations, algorithm) } catch (e: Exception) { continue }
                for (tagLast in listOf(false, true)) {
                    val body = payload.copyOfRange(NONCE, payload.size)
                    val sealed = if (tagLast) body else body.copyOfRange(TAG, body.size) + body.copyOfRange(0, TAG)
                    try {
                        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                        cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG * 8, nonce))
                        return cipher.doFinal(sealed)
                    } catch (e: Exception) {
                        // not this combination
                    }
                }
            }
        }
        return null
    }

    private fun decode(text: String): ByteArray = try {
        Base64.getDecoder().decode(text.trim())
    } catch (e: IllegalArgumentException) {
        try { Base64.getUrlDecoder().decode(text.trim()) } catch (e2: IllegalArgumentException) { ByteArray(0) }
    }
}
