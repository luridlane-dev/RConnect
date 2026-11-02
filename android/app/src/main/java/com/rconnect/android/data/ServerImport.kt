package com.rconnect.android.data

/**
 * Reads a pasted list, one server per line: `ip;user;password;name;group` (name and group are optional).
 * The separator is `;` or a tab (so spreadsheet rows can be pasted); a first line that starts with
 * "ip" or "host" is taken as a header and skipped. Pure code, no Android.
 */
object ServerImport {
    data class Result(val servers: List<ServerEntry>, val skipped: Int)

    fun parse(text: String, existing: List<ServerEntry> = emptyList()): Result {
        val known = existing.map { key(it.host, it.user) }.toMutableSet()
        val added = ArrayList<ServerEntry>()
        var skipped = 0
        for ((i, raw) in text.lines().withIndex()) {
            val line = raw.trim()
            if (line.isEmpty()) continue
            val separator = if (line.contains('\t')) '\t' else ';'
            val f = line.split(separator).map { it.trim() }
            if (i == 0 && (f[0].equals("ip", true) || f[0].equals("host", true) || f[0].equals("ip address", true))) continue
            if (f.size < 3 || f[0].isEmpty() || f[1].isEmpty() || f[2].isEmpty() || !key(f[0], f[1]).let(known::add)) {
                skipped++
                continue
            }
            added += ServerEntry(name = f.getOrElse(3) { "" }, host = f[0], user = f[1], password = f[2], group = f.getOrElse(4) { "" })
        }
        return Result(added, skipped)
    }

    private fun key(host: String, user: String) = host.lowercase() + "|" + user.lowercase()
}
