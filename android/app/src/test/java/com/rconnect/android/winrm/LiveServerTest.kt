package com.rconnect.android.winrm

import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File

/**
 * Talks to a real server; skipped unless RC_HOST, RC_USER and RC_PASS are set (RC_OUT: file for the result).
 * Read only: lists sessions and reads event logs.
 */
class LiveServerTest {
    @Test
    fun readsSessionsAndEvents() {
        val host = System.getenv("RC_HOST")
        val user = System.getenv("RC_USER")
        val pass = System.getenv("RC_PASS")
        assumeTrue(!host.isNullOrBlank() && !user.isNullOrBlank() && !pass.isNullOrBlank())
        val out = StringBuilder()
        try {
            val snapshot = WinRmClient(host!!, user!!, pass!!).snapshot(withEvents = true)
            out.appendLine("OK server clock=${snapshot.serverNowUtcMs}")
            for (s in snapshot.sessions) out.appendLine("session: $s")
            for (e in snapshot.events.take(20)) out.appendLine("event: $e")
            out.appendLine("events total=${snapshot.events.size}")
        } catch (e: WinRmException) {
            out.appendLine("FAILED ${e::class.simpleName}: ${e.message}")
        }
        System.getenv("RC_OUT")?.let { File(it).writeText(out.toString()) }
        println(out)
    }
}
