package com.rconnect.android.winrm

import com.rconnect.android.data.ServerEntry
import com.rconnect.android.data.ServerImport
import org.junit.Assert.assertEquals
import org.junit.Test

class ServerImportTest {
    @Test
    fun readsLinesTabsAndSkipsBadOnes() {
        val text = "ip;user;password;name;group\n" +
            "10.0.0.1;admin;secret;SRV-01;Ankara\n" +
            "10.0.0.2\tadmin\tp;ss\tSRV-02\n" +
            "\n" +
            "10.0.0.3;admin\n" +
            "10.0.0.1;ADMIN;other;dup\n"
        val result = ServerImport.parse(text)
        assertEquals(2, result.servers.size)
        assertEquals(2, result.skipped)
        assertEquals("Ankara", result.servers[0].group)
        assertEquals("p;ss", result.servers[1].password)
    }

    @Test
    fun skipsServersAlreadyInTheList() {
        val existing = listOf(ServerEntry(name = "", host = "10.0.0.1", user = "admin", password = "x"))
        val result = ServerImport.parse("10.0.0.1;admin;secret\n10.0.0.9;admin;secret", existing)
        assertEquals(1, result.servers.size)
        assertEquals(1, result.skipped)
    }
}
