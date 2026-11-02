package com.rconnect.android.data

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
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
) {
    val title get() = name.ifBlank { host }
}

/** Passwords are encrypted with a key that never leaves the phone's keystore. */
private object Vault {
    private const val ALIAS = "rconnect_passwords"

    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return generator.generateKey()
    }

    fun encrypt(plain: String): String {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, key()) }
        val data = cipher.doFinal(plain.toByteArray())
        return Base64.encodeToString(cipher.iv + data, Base64.NO_WRAP)
    }

    fun decrypt(encoded: String): String = try {
        val raw = Base64.decode(encoded, Base64.NO_WRAP)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            .apply { init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, raw, 0, 12)) }
        String(cipher.doFinal(raw, 12, raw.size - 12))
    } catch (e: Exception) {
        "" // key lost (for example after a restore): the person enters the password again
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
        if (get(serverId, address) != name) prefs.edit().putString("$serverId|$address", name).apply()
    }
}

class ServerStore(context: Context) {
    private val prefs = context.getSharedPreferences("servers", Context.MODE_PRIVATE)

    fun load(): List<ServerEntry> = try {
        val array = JSONArray(prefs.getString("list", "[]"))
        (0 until array.length()).map {
            val o = array.getJSONObject(it)
            ServerEntry(o.getString("id"), o.optString("name"), o.getString("host"), o.optString("user"),
                Vault.decrypt(o.optString("password")), o.optString("group"))
        }
    } catch (e: Exception) {
        emptyList()
    }

    fun save(servers: List<ServerEntry>) {
        val array = JSONArray()
        for (s in servers) array.put(JSONObject().put("id", s.id).put("name", s.name).put("host", s.host)
            .put("user", s.user).put("password", Vault.encrypt(s.password)).put("group", s.group))
        prefs.edit().putString("list", array.toString()).apply()
    }
}
