package com.rconnect.android.winrm

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ParsingTest {
    private val quser = """ USERNAME              SESSIONNAME        ID  STATE   IDLE TIME  LOGON TIME
>administrator         rdp-tcp#12          2  Active          .  31.10.2026 08:15
 ali                                       3  Disc         1:05  30.10.2026 22:01
##RCSTART##
NOW 2026-11-02T12:00:00.1234567Z
2 2026-10-31T05:15:30.0000000Z
3 2026-10-30T19:01:00.5000000Z
##RCONNECT##
HKEY_USERS\S-1-5-21-1\Volatile Environment\2
    CLIENTNAME    REG_SZ    PC-ALI
"""

    @Test
    fun sessionsClientsAndSignInTimes() {
        val snapshot = Parsing.snapshot(quser, withEvents = false)
        assertEquals(2, snapshot.sessions.size)
        val a = snapshot.sessions[0]
        assertEquals("administrator", a.user)
        assertEquals(2, a.id)
        assertTrue(a.connected)
        assertEquals("PC-ALI", a.client)
        assertEquals(0, a.idleMinutes)
        assertEquals(1793423730000L, a.logonUtcMs)
        val b = snapshot.sessions[1]
        assertEquals("ali", b.user)
        assertTrue(!b.connected)
        assertEquals(65, b.idleMinutes)
        assertTrue(snapshot.serverNowUtcMs > 0)
    }

    @Test
    fun idleFormats() {
        assertEquals(0, Parsing.parseIdle("."))
        assertEquals(0, Parsing.parseIdle("none"))
        assertEquals(5, Parsing.parseIdle("5"))
        assertEquals(65, Parsing.parseIdle("1:05"))
        assertEquals(2 * 1440 + 3 * 60 + 4, Parsing.parseIdle("2+03:04"))
    }

    @Test
    fun events() {
        val xml = """##RCLOG:LSM##
<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><EventID>21</EventID><TimeCreated SystemTime='2026-10-31T08:15:30.1234567Z'/><EventRecordID>77</EventRecordID></System><UserData><EventXML xmlns='Event_NS'><User>SRV\ali</User><SessionID>3</SessionID><Address>10.0.0.5</Address></EventXML></UserData></Event>
<Event xmlns='x'><System><EventID>23</EventID><TimeCreated SystemTime='2026-10-31T09:00:00Z'/><EventRecordID>78</EventRecordID></System><UserData><EventXML><User>SRV\ali</User><SessionID>3</SessionID><Address>LOCAL</Address></EventXML></UserData></Event>
##RCLOG:SEC##
<Event xmlns='x'><System><EventID>4625</EventID><TimeCreated SystemTime='2026-10-31T10:00:00.5Z'/><EventRecordID>5</EventRecordID></System><EventData><Data Name='TargetUserName'>ADMINISTRAT${'�'}R</Data><Data Name='TargetDomainName'>-</Data><Data Name='Status'>0xc000006d</Data><Data Name='SubStatus'>0xc000006a</Data><Data Name='IpAddress'>1.2.3.4</Data><Data Name='WorkstationName'>-</Data></EventData></Event>
##RCONNECT-EVENTS##
"""
        val events = Parsing.snapshot(xml + "\n header\n", withEvents = true).events
        assertEquals(3, events.size)
        assertEquals(AccessEvent.Kind.FailedSignIn, events[0].kind) // newest first
        assertEquals("wrong password", events[0].detail)
        assertEquals("1.2.3.4", events[0].address)
        assertEquals(AccessEvent.Kind.SignOut, events[1].kind)
        assertEquals(AccessEvent.Kind.SignIn, events[2].kind)
        assertEquals("SRV\\ali", events[2].user)
        assertEquals("10.0.0.5", events[2].address)
        assertEquals(3, events[2].sessionId)
    }

    @Test
    fun commandHasNoUnexpandedTemplates() {
        val c = WinRmClient.sessionCommand(1000)
        assertTrue(c.contains("\$_.SessionId"))
        assertTrue(!c.contains("TIME]"))
        assertTrue(c.startsWith("echo ##RCLOG:LSM##"))
        assertTrue(WinRmClient.sessionCommand(null).startsWith("quser"))
    }
}
