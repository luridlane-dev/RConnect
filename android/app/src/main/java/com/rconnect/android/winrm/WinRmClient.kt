package com.rconnect.android.winrm

import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import java.util.Base64
import java.util.UUID

/**
 * Runs one command on a Windows server over WinRM (HTTP 5985, NTLM with message encryption), the same
 * way the desktop program does. Blocking; call it off the main thread. Needs an administrator account.
 */
class WinRmClient(
    private val host: String,
    private val user: String,
    private val password: String,
    private val domain: String = "",
    private val timeoutMs: Int = 25000,
    private val port: Int = 5985,
) {
    private class Response(val status: Int, val authenticate: List<String>, val contentType: String, val body: ByteArray)

    private lateinit var socket: Socket
    private lateinit var input: InputStream
    private lateinit var output: OutputStream
    private val ntlm = Ntlm(user, domain, password)
    private val hostHeader = (if (host.contains(':')) "[$host]" else host) + ":" + port
    private val to = "http://$hostHeader/wsman"
    private var startedAt = 0L

    /** The server's sessions, and (when [withEvents]) the session events after [lookbackMs] ago. */
    fun snapshot(withEvents: Boolean, lookbackMs: Long = DEFAULT_LOOKBACK_MS): ServerSnapshot {
        val text = run(sessionCommand(if (withEvents) lookbackMs else null))
        return Parsing.snapshot(text, withEvents)
    }

    fun run(command: String): String {
        startedAt = System.currentTimeMillis()
        try {
            connect()
        } catch (e: IOException) {
            // One lost connection attempt is not an outage (mobile networks drop some); try once more.
            try {
                Thread.sleep(RETRY_DELAY_MS)
                connect()
            } catch (e: IOException) {
                throw WinRmException.Unreachable("Could not reach the server's remote management service (port $port).", rdpOpen())
            }
        }
        try {
            authenticate()
            return execute(command)
        } catch (e: SocketTimeoutException) {
            throw WinRmException.Failed("The server did not answer in time.")
        } catch (e: IOException) {
            throw WinRmException.Failed("The connection to the server was lost.")
        } finally {
            try { socket.close() } catch (_: IOException) {}
        }
    }

    // ---- connection -------------------------------------------------------------------------

    private fun connect() {
        socket = Socket()
        socket.connect(InetSocketAddress(host, port), minOf(timeoutMs, 5000))
        socket.soTimeout = timeoutMs
        socket.tcpNoDelay = true
        input = socket.getInputStream().buffered()
        output = socket.getOutputStream()
    }

    /** Whether the server answers on the Remote Desktop port, which is how the desktop program tells it is up. */
    private fun rdpOpen(): Boolean = try {
        Socket().use { it.connect(InetSocketAddress(host, RDP_PORT), 3000) }
        true
    } catch (e: IOException) {
        false
    }

    private fun request(contentType: String, body: ByteArray, auth: ByteArray? = null): ByteArray {
        val head = StringBuilder()
            .append("POST /wsman HTTP/1.1\r\nHost: ").append(hostHeader).append("\r\nUser-Agent: RConnect\r\n")
            .append("Connection: Keep-Alive\r\nContent-Type: ").append(contentType).append("\r\n")
            .append("Content-Length: ").append(body.size).append("\r\n")
        if (auth != null) head.append("Authorization: Negotiate ").append(Base64.getEncoder().encodeToString(auth)).append("\r\n")
        head.append("\r\n")
        return head.toString().toByteArray(Charsets.ISO_8859_1) + body
    }

    private fun readLine(): String {
        val line = ByteArrayOutputStream()
        while (true) {
            val b = input.read()
            if (b < 0) throw IOException("closed")
            if (b == '\n'.code) break
            if (b != '\r'.code) line.write(b)
        }
        return String(line.toByteArray(), Charsets.ISO_8859_1)
    }

    private fun readBytes(n: Int): ByteArray {
        val buf = ByteArray(n)
        var got = 0
        while (got < n) {
            val r = input.read(buf, got, n - got)
            if (r < 0) throw IOException("closed")
            got += r
        }
        return buf
    }

    private fun receive(): Response {
        val status = readLine().split(' ').getOrNull(1)?.toIntOrNull() ?: 0
        val authenticate = ArrayList<String>()
        var contentType = ""
        var length = 0
        var chunked = false
        while (true) {
            val line = readLine()
            if (line.isEmpty()) break
            val colon = line.indexOf(':')
            if (colon < 0) continue
            val name = line.substring(0, colon).trim().lowercase()
            val value = line.substring(colon + 1).trim()
            when (name) {
                "www-authenticate" -> authenticate += value
                "content-type" -> contentType = value
                "content-length" -> length = value.toIntOrNull() ?: 0
                "transfer-encoding" -> chunked = value.lowercase().contains("chunked")
            }
        }
        if (!chunked) return Response(status, authenticate, contentType, readBytes(length))
        val body = ByteArrayOutputStream()
        while (true) {
            val size = readLine().substringBefore(';').trim().toInt(16)
            if (size == 0) { readLine(); break }
            body.write(readBytes(size))
            readLine()
        }
        return Response(status, authenticate, contentType, body.toByteArray())
    }

    // ---- sign-in ----------------------------------------------------------------------------

    private fun authenticate() {
        val type = "application/soap+xml;charset=UTF-8"
        output.write(request(type, ByteArray(0), ntlm.negotiateMessage())); output.flush()
        var resp = receive()
        val challenge = resp.authenticate.firstOrNull { it.startsWith("Negotiate ") }
            ?.let { Base64.getDecoder().decode(it.substring(10).trim()) }
        if (resp.status != 401 || challenge == null)
            throw WinRmException.Failed("The server does not accept this kind of sign-in (code ${resp.status}).")
        val token = try {
            ntlm.authenticateMessage(challenge)
        } catch (e: IllegalArgumentException) {
            throw WinRmException.Failed(e.message ?: "NTLM sign-in failed.")
        }
        output.write(request(type, ByteArray(0), token)); output.flush()
        resp = receive()
        if (resp.status == 401)
            throw WinRmException.AuthFailed("Sign-in was refused. Use one of the server's administrator accounts.")
    }

    // ---- shell ------------------------------------------------------------------------------

    private fun execute(command: String): String {
        val options = "<w:OptionSet><w:Option Name=\"WINRS_NOPROFILE\">TRUE</w:Option>" +
            "<w:Option Name=\"WINRS_CODEPAGE\">65001</w:Option></w:OptionSet>"
        val shell = "<rsp:Shell><rsp:InputStreams>stdin</rsp:InputStreams>" +
            "<rsp:OutputStreams>stdout stderr</rsp:OutputStreams></rsp:Shell>"
        val created = call("$TRANSFER_NS/Create", "", options, shell)
        val shellId = created.shellId.ifEmpty { throw WinRmException.Failed("The server did not open a shell.") }

        val out = ByteArrayOutputStream()
        try {
            val cmdOptions = "<w:OptionSet><w:Option Name=\"WINRS_CONSOLEMODE_STDIN\">TRUE</w:Option>" +
                "<w:Option Name=\"WINRS_SKIP_CMD_SHELL\">FALSE</w:Option></w:OptionSet>"
            val commandId = call("$SHELL_NS/Command", shellId, cmdOptions,
                "<rsp:CommandLine><rsp:Command>${escape(command)}</rsp:Command></rsp:CommandLine>").commandId
            val receive = "<rsp:Receive><rsp:DesiredStream CommandId=\"$commandId\">stdout stderr</rsp:DesiredStream></rsp:Receive>"
            var done = false
            while (!done && System.currentTimeMillis() - startedAt < timeoutMs) {
                val r = call("$SHELL_NS/Receive", shellId, "", receive, timeoutIsOk = true)
                out.write(r.stdout)
                done = r.done
            }
            if (!done) throw WinRmException.Failed("The server did not answer in time.")
        } finally {
            // Shells count against a per-user limit on the server.
            try { call("$TRANSFER_NS/Delete", shellId, "", "") } catch (_: Exception) {}
        }
        return String(out.toByteArray(), Charsets.UTF_8)
    }

    private class Reply(val shellId: String, val commandId: String, val stdout: ByteArray, val done: Boolean, val faultCode: String, val faultText: String)

    private fun envelope(action: String, shellId: String, options: String, body: String): ByteArray {
        val selector = if (shellId.isEmpty()) "" else "<w:SelectorSet><w:Selector Name=\"ShellId\">${escape(shellId)}</w:Selector></w:SelectorSet>"
        val seconds = maxOf(3, timeoutMs / 1000 - 1)
        return ("<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" " +
            "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" " +
            "xmlns:w=\"http://schemas.dmtf.org/wbem/wsman/1/wsman.xsd\" " +
            "xmlns:p=\"http://schemas.microsoft.com/wbem/wsman/1/wsman.xsd\" " +
            "xmlns:rsp=\"http://schemas.microsoft.com/wbem/wsman/1/windows/shell\">" +
            "<s:Header><a:To>${escape(to)}</a:To>" +
            "<a:ReplyTo><a:Address s:mustUnderstand=\"true\">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:Address></a:ReplyTo>" +
            "<w:MaxEnvelopeSize s:mustUnderstand=\"true\">153600</w:MaxEnvelopeSize>" +
            "<a:MessageID>uuid:${UUID.randomUUID().toString().uppercase()}</a:MessageID>" +
            "<w:Locale xml:lang=\"en-US\" s:mustUnderstand=\"false\"/><p:DataLocale xml:lang=\"en-US\" s:mustUnderstand=\"false\"/>" +
            "<w:OperationTimeout>PT${seconds}S</w:OperationTimeout>" +
            "<w:ResourceURI s:mustUnderstand=\"true\">$SHELL_URI</w:ResourceURI>" +
            "<a:Action s:mustUnderstand=\"true\">$action</a:Action>$selector$options</s:Header>" +
            "<s:Body>$body</s:Body></s:Envelope>").toByteArray(Charsets.UTF_8)
    }

    private fun call(action: String, shellId: String, options: String, body: String, timeoutIsOk: Boolean = false): Reply {
        val plain = envelope(action, shellId, options, body)
        val (signature, sealed) = ntlm.seal(plain)
        val payload = ByteArrayOutputStream().apply {
            write("$BOUNDARY\r\n\tContent-Type: $PROTOCOL\r\n\tOriginalContent: type=application/soap+xml;charset=UTF-8;Length=${plain.size}\r\n$BOUNDARY\r\n".toByteArray())
            write(OCTET_HEADER)
            write(Ntlm.le32(signature.size)); write(signature); write(sealed)
            write("$BOUNDARY--\r\n".toByteArray())
        }.toByteArray()
        val contentType = "multipart/encrypted;protocol=\"$PROTOCOL\";boundary=\"Encrypted Boundary\""
        output.write(request(contentType, payload)); output.flush()
        val resp = receive()
        if (resp.status == 401)
            throw WinRmException.AuthFailed("Sign-in was refused. Use one of the server's administrator accounts.")

        var xml = resp.body
        if (resp.contentType.contains("multipart/encrypted")) {
            val start = indexOf(resp.body, OCTET_HEADER)
            val end = lastIndexOf(resp.body, "$BOUNDARY--".toByteArray())
            if (start < 0 || end < start) throw WinRmException.Failed("Could not read the encrypted reply.")
            val blob = resp.body.copyOfRange(start + OCTET_HEADER.size, end)
            val sigLen = if (blob.size >= 4) (blob[0].toInt() and 0xFF) or ((blob[1].toInt() and 0xFF) shl 8) or
                ((blob[2].toInt() and 0xFF) shl 16) or ((blob[3].toInt() and 0xFF) shl 24) else -1
            if (sigLen != 16 || blob.size < 4 + sigLen) throw WinRmException.Failed("Could not decrypt the reply.")
            val (plainReply, ok) = ntlm.unseal(blob.copyOfRange(4, 4 + sigLen), blob.copyOfRange(4 + sigLen, blob.size))
            if (!ok) throw WinRmException.Failed("The reply failed its integrity check.")
            xml = plainReply
        }
        val reply = parseReply(String(xml, Charsets.UTF_8))
        if (resp.status == 200) return reply
        if (timeoutIsOk && reply.faultCode == TIMED_OUT_CODE) return Reply("", "", ByteArray(0), false, reply.faultCode, reply.faultText)
        throw WinRmException.Failed(reply.faultText.ifEmpty { "The server returned an error (code ${resp.status})." })
    }

    private fun parseReply(xml: String): Reply {
        fun first(re: Regex) = re.find(xml)?.groupValues?.get(1).orEmpty()
        val shellId = first(Regex("<[\\w:]*ShellId>([^<]*)</[\\w:]*ShellId>")).ifEmpty {
            first(Regex("<[\\w:]*Selector Name=\"ShellId\">([^<]*)</[\\w:]*Selector>"))
        }
        val stdout = ByteArrayOutputStream()
        for (m in Regex("<[\\w:]*Stream\\s+([^>]*)>([^<]*)</[\\w:]*Stream>").findAll(xml))
            if (m.groupValues[1].contains("Name=\"stdout\"")) stdout.write(Base64.getMimeDecoder().decode(m.groupValues[2]))
        return Reply(
            shellId = shellId,
            commandId = first(Regex("<[\\w:]*CommandId>([^<]*)</[\\w:]*CommandId>")),
            stdout = stdout.toByteArray(),
            done = Regex("<[\\w:]*CommandState[^>]*State=\"[^\"]*/Done\"").containsMatchIn(xml),
            faultCode = first(Regex("<[\\w:]*WSManFault[^>]*Code=\"(\\d+)\"")),
            faultText = first(Regex("<[\\w:]*(?:Message|Text)[^>]*>([^<]*)</[\\w:]*(?:Message|Text)>")).trim().replace(Regex("\\s+"), " "),
        )
    }

    companion object {
        const val DEFAULT_LOOKBACK_MS = 2L * 24 * 3600 * 1000
        private const val RETRY_DELAY_MS = 1500L
        private const val RDP_PORT = 3389
        private const val BOUNDARY = "--Encrypted Boundary"
        private const val PROTOCOL = "application/HTTP-SPNEGO-session-encrypted"
        private val OCTET_HEADER = "\tContent-Type: application/octet-stream\r\n".toByteArray()
        private const val SHELL_URI = "http://schemas.microsoft.com/wbem/wsman/1/windows/shell/cmd"
        private const val SHELL_NS = "http://schemas.microsoft.com/wbem/wsman/1/windows/shell"
        private const val TRANSFER_NS = "http://schemas.xmlsoap.org/ws/2004/09/transfer"
        // Receive timed out with no output yet; ask again.
        private const val TIMED_OUT_CODE = "2150858793"
        private const val MAX_EVENTS = 100

        fun escape(s: String) = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;")

        private fun indexOf(data: ByteArray, pattern: ByteArray): Int {
            outer@ for (i in 0..data.size - pattern.size) {
                for (j in pattern.indices) if (data[i + j] != pattern[j]) continue@outer
                return i
            }
            return -1
        }

        private fun lastIndexOf(data: ByteArray, pattern: ByteArray): Int {
            outer@ for (i in data.size - pattern.size downTo 0) {
                for (j in pattern.indices) if (data[i + j] != pattern[j]) continue@outer
                return i
            }
            return -1
        }

        private fun eventLog(source: String, log: String, xpath: String, lookbackMs: Long) =
            "echo ${Parsing.LOG_MARKER}$source## & wevtutil qe $log /c:$MAX_EVENTS /rd:true /f:xml " +
                "/q:\"${xpath.replace("TIME", "timediff(@SystemTime)<=$lookbackMs")}\" 2>nul & "

        /**
         * Session list, and (with [lookbackMs]) the newest sign-in/out events of the session log and failed
         * sign-ins. The events come first, because `for` takes the rest of the line as its body.
         */
        fun sessionCommand(lookbackMs: Long?): String {
            val events = if (lookbackMs == null) "" else
                eventLog("LSM", "Microsoft-Windows-TerminalServices-LocalSessionManager/Operational",
                    "*[System[(EventID=21 or EventID=23 or EventID=24 or EventID=25) and TimeCreated[TIME]]]", lookbackMs) +
                    eventLog("SEC", "Security",
                        "*[System[EventID=4625 and TimeCreated[TIME]] and EventData[Data[@Name='LogonType']='10' or Data[@Name='LogonType']='3']]", lookbackMs) +
                    eventLog("LOGON", "Security",
                        "*[System[(EventID=4778 or EventID=4779) and TimeCreated[TIME]]]", lookbackMs) +
                    "echo ${Parsing.EVENTS_MARKER} & "
            // The sign-in time of each session is when its shell (explorer) started, as UTC, so the server's
            // time zone and the language of quser do not matter.
            return events + "quser & echo ${Parsing.START_MARKER} & powershell -NoProfile -NonInteractive -Command " +
                "\"'NOW ' + [DateTime]::UtcNow.ToString('o'); " +
                "Get-Process explorer -ErrorAction SilentlyContinue | ForEach-Object { '{0} {1}' -f " +
                "\$_.SessionId, \$_.StartTime.ToUniversalTime().ToString('o') }\" 2>nul" +
                " & echo ${Parsing.MARKER} & for /f \"delims=\" %k in ('reg query HKU') do " +
                "@reg query \"%k\\Volatile Environment\" /s /v CLIENTNAME 2>nul"
        }
    }
}
