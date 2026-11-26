package com.rconnect.android.data

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import androidx.core.content.edit
import org.json.JSONArray
import org.json.JSONObject
import java.security.KeyStore
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** A server the person added: just where it is and which administrator account to use. */
data class ServerEntry(
    val id: String = UUID.randomUUID().toString(),
    val name: String,
    val host: String,
    val user: String,
    val password: String,
    val group: String = "",
    /** Remote Desktop port, used for reachability checks and kept on export. */
    val port: Int = 3389,
    val favorite: Boolean = false,
    /** Fields of an imported file the phone does not use (colour, notes, ...), kept as JSON so an export gives them back. */
    val extra: String = "",
) {
    val title get() = name.ifBlank { host }
}

/** Passwords are encrypted with a key that never leaves the phone's keystore. */
private object Vault {
    private const val ALIAS = "rconnect_passwords"
    private const val IV_BYTES = 12
    private const val TAG_BYTES = 16
    @Volatile
    private var cachedKey: SecretKey? = null

    @Synchronized
    private fun key(recreate: Boolean = false): SecretKey {
        if (!recreate) cachedKey?.let { return it }
        cachedKey = null
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        if (recreate) {
            runCatching { store.deleteEntry(ALIAS) }
        } else {
            runCatching { store.getKey(ALIAS, null) as? SecretKey }.getOrNull()?.let {
                cachedKey = it
                return it
            }
            runCatching { if (store.containsAlias(ALIAS)) store.deleteEntry(ALIAS) }
        }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return generator.generateKey().also { cachedKey = it }
    }

    @Synchronized
    fun encrypt(plain: String): String {
        if (plain.isEmpty()) return ""
        return try {
            seal(plain, key())
        } catch (_: Exception) {
            // The keystore key can be invalidated after a lock-screen or OS change; create a fresh one.
            runCatching { seal(plain, key(recreate = true)) }.getOrDefault("")
        }
    }

    private fun seal(plain: String, secret: SecretKey): String {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, secret) }
        val data = cipher.doFinal(plain.toByteArray(Charsets.UTF_8))
        return Base64.encodeToString(cipher.iv + data, Base64.NO_WRAP)
    }

    @Synchronized
    fun decrypt(encoded: String): String {
        if (encoded.isEmpty()) return ""
        return try {
            val raw = Base64.decode(encoded, Base64.NO_WRAP)
            if (raw.size < IV_BYTES + TAG_BYTES) return ""
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                .apply { init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(TAG_BYTES * 8, raw, 0, IV_BYTES)) }
            String(cipher.doFinal(raw, IV_BYTES, raw.size - IV_BYTES), Charsets.UTF_8)
        } catch (_: Exception) {
            "" // key lost (for example after a restore): the person enters the password again
        }
    }
}

/**
 * Computer names the phone has seen for a client address on a server. The server only tells the name of
 * connected sessions, so the name is remembered to label the same address in the history.
 */
class ClientNames(context: Context) {
    private val prefs = context.getSharedPreferences("client_names", Context.MODE_PRIVATE)

    fun get(serverId: String, address: String): String = prefs.getString("$serverId|$address", "").orEmpty()

    fun put(serverId: String, address: String, name: String) {
        if (get(serverId, address) != name) prefs.edit { putString("$serverId|$address", name) }
    }
}

class ServerStore(context: Context) {
    private val prefs = context.getSharedPreferences("servers", Context.MODE_PRIVATE)
    // Caches (plaintext -> ciphertext) per server id so non-password edits (favorite, group) don't re-encrypt every server.
    private val encryptedPasswords = HashMap<String, Pair<String, String>>()

    // One damaged entry must not take the whole list with it: the next save would write the shortened list back.
    @Synchronized
    fun load(): List<ServerEntry> {
        val array = try { JSONArray(prefs.getString("list", "[]")) } catch (_: Exception) { return emptyList() }
        encryptedPasswords.clear()
        return (0 until array.length()).mapNotNull {
            try {
                val o = array.getJSONObject(it)
                val extraStr = o.optString("extra")
                val favorite = if (o.has("favorite")) o.optBoolean("favorite", false)
                else runCatching { JSONObject(extraStr).optBoolean("IsFavorite", false) }.getOrDefault(false)
                val id = o.getString("id")
                val encPw = o.optString("password")
                val plainPw = Vault.decrypt(encPw)
                if (plainPw.isNotEmpty() && encPw.isNotEmpty()) {
                    encryptedPasswords[id] = plainPw to encPw
                }
                ServerEntry(
                    id = id,
                    name = o.optString("name"),
                    host = o.getString("host"),
                    user = o.optString("user"),
                    password = plainPw,
                    group = o.optString("group"),
                    port = o.optInt("port", 3389).takeIf { p -> p in 1..65535 } ?: 3389,
                    favorite = favorite,
                    extra = extraStr,
                )
            } catch (_: Exception) {
                null
            }
        }
    }

    @Synchronized
    fun save(servers: List<ServerEntry>) {
        val array = JSONArray()
        val nextCache = HashMap<String, Pair<String, String>>(servers.size)
        for (s in servers) {
            val encPw = if (s.password.isEmpty()) {
                ""
            } else {
                val cached = encryptedPasswords[s.id]
                if (cached != null && cached.first == s.password) cached.second
                else Vault.encrypt(s.password)
            }
            if (s.password.isNotEmpty() && encPw.isNotEmpty()) {
                nextCache[s.id] = s.password to encPw
            }
            array.put(
                JSONObject()
                    .put("id", s.id)
                    .put("name", s.name)
                    .put("host", s.host)
                    .put("user", s.user)
                    .put("password", encPw)
                    .put("group", s.group)
                    .put("port", s.port)
                    .put("favorite", s.favorite)
                    .put("extra", s.extra),
            )
        }
        encryptedPasswords.clear()
        encryptedPasswords.putAll(nextCache)
        prefs.edit(commit = true) { putString("list", array.toString()) }
    }
}
