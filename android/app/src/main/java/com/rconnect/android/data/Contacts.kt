package com.rconnect.android.data

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

/**
 * A person in the address book: a nickname for a computer, for example the computer "LENOVO" is "Elif".
 * The computer name identifies the person, so the nickname follows them from any IP address. The account is
 * optional; when both are filled in, both must match.
 */
data class Contact(
    val id: String = UUID.randomUUID().toString(),
    val name: String = "",
    val device: String = "",
    val user: String = "",
) {
    val hasMatch get() = device.isNotBlank() || user.isNotBlank()
    val valid get() = name.isNotBlank() && hasMatch
}

object ContactBook {
    private fun norm(s: String) = s.trim().lowercase()
    private fun shortUser(s: String) = norm(s.substringAfterLast('\\'))

    /** The entry that fits best: the one with the most fields filled in, all of which match. */
    fun match(list: List<Contact>, device: String, user: String): Contact? {
        val d = norm(device)
        val u = shortUser(user)
        var best: Contact? = null
        var bestFields = 0
        for (c in list) {
            var fields = 0
            if (c.device.isNotBlank()) { if (norm(c.device) != d) continue; fields++ }
            if (c.user.isNotBlank()) { if (shortUser(c.user) != u) continue; fields++ }
            if (fields > bestFields) { best = c; bestFields = fields }
        }
        return best
    }

    fun nameFor(list: List<Contact>, device: String, user: String): String? =
        match(list, device, user)?.name

    /** The entry to edit for a session or an event: the one that matches, else a new one filled in with what is known. */
    fun draft(list: List<Contact>, device: String, user: String): Contact =
        match(list, device, user) ?: Contact(device = device)

    /** Whether a long press can name this person: a known one, or one with a computer name to tell them by. */
    fun canName(list: List<Contact>, device: String, user: String) = device.isNotBlank() || match(list, device, user) != null

    /** Same computer and account as [b], whatever the id is. */
    fun sameMatch(a: Contact, b: Contact) = norm(a.device) == norm(b.device) && shortUser(a.user) == shortUser(b.user)
}

/** Reads and writes the list in the file format shared with the Windows program ("rconnect-contacts"). */
object ContactJson {
    private const val FORMAT = "rconnect-contacts"

    fun write(list: List<Contact>): String {
        val array = JSONArray()
        for (c in list) array.put(JSONObject().put("Id", c.id).put("Name", c.name).put("Device", c.device)
            .put("User", c.user))
        return JSONObject().put("Format", FORMAT).put("Version", 1).put("Contacts", array).toString(2)
    }

    /** Null when the text is not an address book. */
    fun read(text: String): List<Contact>? = try {
        val root = JSONObject(text)
        if (root.optString("Format") != FORMAT) null
        else {
            val array = root.optJSONArray("Contacts") ?: JSONArray()
            (0 until array.length()).map { array.getJSONObject(it) }.map {
                Contact(
                    id = it.optString("Id").ifBlank { UUID.randomUUID().toString() },
                    name = it.optString("Name").trim(), device = it.optString("Device").trim(), user = it.optString("User").trim(),
                )
            }.filter { it.valid }
        }
    } catch (e: Exception) {
        null
    }
}

class ContactStore(context: Context) {
    private val prefs = context.getSharedPreferences("contacts", Context.MODE_PRIVATE)

    fun load(): List<Contact> = ContactJson.read(prefs.getString("list", "").orEmpty()).orEmpty()

    fun save(list: List<Contact>) {
        prefs.edit().putString("list", ContactJson.write(list)).apply()
    }
}
