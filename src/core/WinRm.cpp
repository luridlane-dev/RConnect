#include "WinRm.h"

#include "Log.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QThread>
#include <QUuid>
#include <QXmlStreamReader>

#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>
#define SECURITY_WIN32
#include <security.h>

namespace {

constexpr quint16 kPort = 5985;
const QByteArray kBoundary = "--Encrypted Boundary";
const QByteArray kProtocol = "application/HTTP-SPNEGO-session-encrypted";
const QByteArray kOctetHeader = "\tContent-Type: application/octet-stream\r\n";
const char *const kShellUri = "http://schemas.microsoft.com/wbem/wsman/1/windows/shell/cmd";
const char *const kShellNs = "http://schemas.microsoft.com/wbem/wsman/1/windows/shell";
const char *const kTransferNs = "http://schemas.xmlsoap.org/ws/2004/09/transfer";
// Receive timed out with no output yet; ask again.
const char *const kTimedOutCode = "2150858793";
const QByteArray kMarker = "##RCONNECT##";
const QByteArray kEventsMarker = "##RCONNECT-EVENTS##";
// Starts one log's output ("##RCLOG:LSM##"), so each log is parsed on its own.
const QByteArray kLogMarker = "##RCLOG:";
constexpr int kMaxEvents = 300; // per log and call
constexpr qint64 kMaxLookbackMs = 7LL * 24 * 3600 * 1000;

QString timeClause(const EventCursor &c)
{
    // Only a time in the exact format the server writes ends up in the command line.
    static const QRegularExpression format(
        QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d(\\.\\d{1,9})?Z$"));
    if (format.match(c.afterUtc).hasMatch())
        return QStringLiteral("@SystemTime>='%1'").arg(c.afterUtc);
    const qint64 ms = c.lookbackMs > 0 ? qMin(c.lookbackMs, kMaxLookbackMs) : kMaxLookbackMs;
    return QStringLiteral("timediff(@SystemTime)<=%1").arg(ms);
}

QString eventLogQuery(const QString &source, const QString &log, QString xpath, const EventCursor &cursor)
{
    xpath.replace(QLatin1String("TIME"), timeClause(cursor));
    return QStringLiteral("echo %1%2## & wevtutil qe %3 /c:%4 /f:xml /q:\"%5\" 2>nul & ")
        .arg(QString::fromLatin1(kLogMarker), source, log).arg(kMaxEvents).arg(xpath);
}

// Session changes, authentications, and (when the server audits them) RDP logons, failed logons
// and disconnects.
QString eventCommand(const QHash<QString, EventCursor> &cursors)
{
    const QString lsm = QStringLiteral("*[System[(EventID=21 or EventID=23 or EventID=24 or EventID=25) "
                                       "and TimeCreated[TIME]]]");
    const QString rcm = QStringLiteral("*[System[EventID=1149 and TimeCreated[TIME]]]");
    const QString sec = QStringLiteral(
        "*[(System[(EventID=4778 or EventID=4779) and TimeCreated[TIME]]) "
        "or (System[EventID=4624 and TimeCreated[TIME]] and EventData[Data[@Name='LogonType']='10']) "
        "or (System[EventID=4625 and TimeCreated[TIME]] and EventData[Data[@Name='LogonType']='10' "
        "or Data[@Name='LogonType']='3']) "
        // new account, task, cleared log; a member added to Administrators or Remote Desktop Users
        "or (System[(EventID=4720 or EventID=4698 or EventID=1102) and TimeCreated[TIME]]) "
        "or (System[EventID=4732 and TimeCreated[TIME]] and EventData[Data[@Name='TargetSid']='S-1-5-32-544' "
        "or Data[@Name='TargetSid']='S-1-5-32-555'])]");
    const QString sys = QStringLiteral(
        "*[(System[Provider[@Name='Service Control Manager'] and EventID=7045 and TimeCreated[TIME]]) "
        "or (System[Provider[@Name='Microsoft-Windows-Eventlog'] and EventID=104 and TimeCreated[TIME]])]");
    return eventLogQuery(QStringLiteral("LSM"),
                         QStringLiteral("Microsoft-Windows-TerminalServices-LocalSessionManager/Operational"), lsm,
                         cursors.value(QStringLiteral("LSM")))
         + eventLogQuery(QStringLiteral("RCM"),
                         QStringLiteral("Microsoft-Windows-TerminalServices-RemoteConnectionManager/Operational"), rcm,
                         cursors.value(QStringLiteral("RCM")))
         + eventLogQuery(QStringLiteral("SEC"), QStringLiteral("Security"), sec, cursors.value(QStringLiteral("SEC")))
         + eventLogQuery(QStringLiteral("SYS"), QStringLiteral("System"), sys, cursors.value(QStringLiteral("SYS")))
         + QStringLiteral("echo %1 & ").arg(QString::fromLatin1(kEventsMarker));
}

// Sessions, then the client name of each session from the users' volatile environment. The access
// events (when asked for) come first, because `for` takes the rest of the line as its body.
QString sessionCommand(const QHash<QString, EventCursor> *cursors)
{
    return (cursors ? eventCommand(*cursors) : QString())
         // The sign-in time of each session: when its shell (explorer) started, as UTC, so the server's time
         // zone and the language of quser do not matter.
         + QStringLiteral("quser & echo ##RCSTART## & powershell -NoProfile -NonInteractive -Command "
                          "\"'NOW ' + [DateTime]::UtcNow.ToString('o'); "
                          "Get-Process explorer -ErrorAction SilentlyContinue | ForEach-Object { '{0} {1}' -f "
                          "$_.SessionId, $_.StartTime.ToUniversalTime().ToString('o') }\" 2>nul"
                          " & echo ##RCONNECT## & for /f \"delims=\" %k in ('reg query HKU') do "
                          "@reg query \"%k\\Volatile Environment\" /s /v CLIENTNAME 2>nul");
}

struct HttpResponse {
    int status = 0;
    QList<QByteArray> authenticate;
    QByteArray contentType;
    QByteArray body;
};

class Connection {
public:
    ~Connection()
    {
        if (m_socket != INVALID_SOCKET)
            closesocket(m_socket);
    }

    bool open(const QString &host, int connectMs, int ioMs)
    {
        ADDRINFOW hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        ADDRINFOW *results = nullptr;
        if (GetAddrInfoW(reinterpret_cast<const wchar_t *>(host.utf16()), std::to_wstring(kPort).c_str(), &hints,
                         &results) != 0)
            return false;
        for (ADDRINFOW *a = results; a && m_socket == INVALID_SOCKET; a = a->ai_next) {
            SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == INVALID_SOCKET)
                continue;
            u_long mode = 1;
            ioctlsocket(s, FIONBIO, &mode);
            bool ok = connect(s, a->ai_addr, int(a->ai_addrlen)) == 0;
            if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set writable, failed;
                FD_ZERO(&writable);
                FD_ZERO(&failed);
                FD_SET(s, &writable);
                FD_SET(s, &failed);
                timeval tv{connectMs / 1000, (connectMs % 1000) * 1000};
                ok = select(0, nullptr, &writable, &failed, &tv) > 0 && FD_ISSET(s, &writable);
            }
            if (!ok) {
                closesocket(s);
                continue;
            }
            mode = 0;
            ioctlsocket(s, FIONBIO, &mode);
            const DWORD ms = DWORD(ioMs);
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof ms);
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms), sizeof ms);
            m_socket = s;
        }
        FreeAddrInfoW(results);
        return m_socket != INVALID_SOCKET;
    }

    bool send(const QByteArray &data)
    {
        qsizetype sent = 0;
        while (sent < data.size()) {
            const int n = ::send(m_socket, data.constData() + sent, int(data.size() - sent), 0);
            if (n <= 0)
                return false;
            sent += n;
        }
        return true;
    }

    bool receive(HttpResponse &r)
    {
        qsizetype end;
        while ((end = m_buffer.indexOf("\r\n\r\n")) < 0)
            if (!fill())
                return false;
        const QList<QByteArray> lines = m_buffer.left(end).split('\n');
        m_buffer.remove(0, end + 4);

        const QList<QByteArray> statusLine = lines.value(0).trimmed().split(' ');
        r = {};
        r.status = statusLine.value(1).toInt();
        qsizetype length = 0;
        bool chunked = false;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray &line = lines.at(i);
            const qsizetype colon = line.indexOf(':');
            if (colon < 0)
                continue;
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (name == "www-authenticate")
                r.authenticate << value;
            else if (name == "content-type")
                r.contentType = value;
            else if (name == "content-length")
                length = value.toLongLong();
            else if (name == "transfer-encoding")
                chunked = value.toLower().contains("chunked");
        }

        if (!chunked)
            return read(length, r.body);
        for (;;) {
            QByteArray sizeLine;
            if (!readLine(sizeLine))
                return false;
            const qsizetype size = sizeLine.split(';').value(0).trimmed().toLongLong(nullptr, 16);
            QByteArray crlf;
            if (size == 0)
                return readLine(crlf);
            QByteArray chunk;
            if (!read(size, chunk) || !readLine(crlf))
                return false;
            r.body += chunk;
        }
    }

private:
    bool fill()
    {
        char buf[16384];
        const int n = recv(m_socket, buf, sizeof buf, 0);
        if (n <= 0)
            return false;
        m_buffer.append(buf, n);
        return true;
    }

    bool read(qsizetype size, QByteArray &out)
    {
        while (m_buffer.size() < size)
            if (!fill())
                return false;
        out = m_buffer.left(size);
        m_buffer.remove(0, size);
        return true;
    }

    bool readLine(QByteArray &out)
    {
        qsizetype end;
        while ((end = m_buffer.indexOf("\r\n")) < 0)
            if (!fill())
                return false;
        out = m_buffer.left(end);
        m_buffer.remove(0, end + 2);
        return true;
    }

    SOCKET m_socket = INVALID_SOCKET;
    QByteArray m_buffer;
};

// NTLM through SSPI. WinRM accepts raw NTLM tokens under the Negotiate scheme.
class Ntlm {
public:
    ~Ntlm()
    {
        if (m_hasContext)
            DeleteSecurityContext(&m_context);
        if (m_hasCredentials)
            FreeCredentialsHandle(&m_credentials);
    }

    bool init(const QString &host, QString user, QString domain, QString password)
    {
        // DOMAIN\user in the user field
        const qsizetype slash = user.indexOf(QLatin1Char('\\'));
        if (slash >= 0) {
            domain = user.left(slash);
            user = user.mid(slash + 1);
        }
        m_target = L"HTTP/" + host.toStdWString();

        SEC_WINNT_AUTH_IDENTITY_W id{};
        id.User = reinterpret_cast<unsigned short *>(user.data());
        id.UserLength = ULONG(user.size());
        id.Domain = reinterpret_cast<unsigned short *>(domain.data());
        id.DomainLength = ULONG(domain.size());
        id.Password = reinterpret_cast<unsigned short *>(password.data());
        id.PasswordLength = ULONG(password.size());
        id.Flags = SEC_WINNT_AUTH_IDENTITY_UNICODE;
        TimeStamp expiry;
        const SECURITY_STATUS st = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t *>(L"NTLM"),
                                                             SECPKG_CRED_OUTBOUND, nullptr, &id, nullptr, nullptr,
                                                             &m_credentials, &expiry);
        SecureZeroMemory(password.data(), size_t(password.size()) * sizeof(QChar));
        m_hasCredentials = st == SEC_E_OK;
        return m_hasCredentials;
    }

    // `in` is empty for the first token.
    bool step(const QByteArray &in, QByteArray &out)
    {
        SecBuffer inBuf{ULONG(in.size()), SECBUFFER_TOKEN, const_cast<char *>(in.constData())};
        SecBufferDesc inDesc{SECBUFFER_VERSION, 1, &inBuf};
        SecBuffer outBuf{0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc outDesc{SECBUFFER_VERSION, 1, &outBuf};
        ULONG attrs = 0;
        TimeStamp expiry;
        const ULONG flags = ISC_REQ_CONFIDENTIALITY | ISC_REQ_INTEGRITY | ISC_REQ_SEQUENCE_DETECT
                          | ISC_REQ_REPLAY_DETECT | ISC_REQ_ALLOCATE_MEMORY;
        const SECURITY_STATUS st = InitializeSecurityContextW(
            &m_credentials, m_hasContext ? &m_context : nullptr, m_target.data(), flags, 0, SECURITY_NATIVE_DREP,
            m_hasContext ? &inDesc : nullptr, 0, &m_context, &outDesc, &attrs, &expiry);
        if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED)
            return false;
        m_hasContext = true;
        out = QByteArray(static_cast<const char *>(outBuf.pvBuffer), qsizetype(outBuf.cbBuffer));
        if (outBuf.pvBuffer)
            FreeContextBuffer(outBuf.pvBuffer);
        return true;
    }

    // Signature length (4 bytes, little endian), signature, sealed data.
    bool seal(const QByteArray &plain, QByteArray &out)
    {
        SecPkgContext_Sizes sizes{};
        if (QueryContextAttributesW(&m_context, SECPKG_ATTR_SIZES, &sizes) != SEC_E_OK)
            return false;
        QByteArray signature(qsizetype(sizes.cbSecurityTrailer), '\0');
        QByteArray data = plain;
        SecBuffer bufs[2] = {{ULONG(signature.size()), SECBUFFER_TOKEN, signature.data()},
                             {ULONG(data.size()), SECBUFFER_DATA, data.data()}};
        SecBufferDesc desc{SECBUFFER_VERSION, 2, bufs};
        if (EncryptMessage(&m_context, 0, &desc, 0) != SEC_E_OK)
            return false;
        signature.truncate(qsizetype(bufs[0].cbBuffer));
        const quint32 len = quint32(signature.size());
        out = QByteArray(reinterpret_cast<const char *>(&len), 4) + signature + data.left(qsizetype(bufs[1].cbBuffer));
        return true;
    }

    bool unseal(const QByteArray &in, QByteArray &plain)
    {
        if (in.size() < 4)
            return false;
        quint32 len = 0;
        memcpy(&len, in.constData(), 4);
        if (in.size() < 4 + qsizetype(len))
            return false;
        QByteArray signature = in.mid(4, len);
        QByteArray data = in.mid(4 + len);
        SecBuffer bufs[2] = {{ULONG(signature.size()), SECBUFFER_TOKEN, signature.data()},
                             {ULONG(data.size()), SECBUFFER_DATA, data.data()}};
        SecBufferDesc desc{SECBUFFER_VERSION, 2, bufs};
        ULONG qop = 0;
        if (DecryptMessage(&m_context, &desc, 0, &qop) != SEC_E_OK)
            return false;
        plain = data.left(qsizetype(bufs[1].cbBuffer));
        return true;
    }

private:
    CredHandle m_credentials{};
    CtxtHandle m_context{};
    bool m_hasCredentials = false;
    bool m_hasContext = false;
    std::wstring m_target;
};

struct Reply {
    QString shellId;
    QString commandId;
    QByteArray stdOut;
    QByteArray stdErr;
    bool done = false;
    QString faultCode;
    QString faultText;
};

Reply parseReply(const QByteArray &xml)
{
    Reply r;
    QXmlStreamReader x(xml);
    while (!x.atEnd()) {
        if (x.readNext() != QXmlStreamReader::StartElement)
            continue;
        const QStringView name = x.name();
        if (name == u"ShellId") {
            r.shellId = x.readElementText();
        } else if (name == u"Selector" && x.attributes().value(u"Name") == u"ShellId" && r.shellId.isEmpty()) {
            r.shellId = x.readElementText();
        } else if (name == u"CommandId") {
            r.commandId = x.readElementText();
        } else if (name == u"Stream") {
            const bool isStdOut = x.attributes().value(u"Name") == u"stdout";
            const QByteArray data = QByteArray::fromBase64(x.readElementText().toLatin1());
            if (isStdOut)
                r.stdOut += data;
            else
                r.stdErr += data;
        } else if (name == u"CommandState") {
            r.done |= x.attributes().value(u"State").endsWith(u"/Done");
        } else if (name == u"WSManFault") {
            r.faultCode = x.attributes().value(u"Code").toString();
        } else if ((name == u"Message" || name == u"Text") && r.faultText.isEmpty()) {
            r.faultText = x.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
        }
    }
    return r;
}

QString xmlEscaped(const QString &s)
{
    return s.toHtmlEscaped();
}

class Client {
public:
    Client(const QString &host, int timeoutMs)
        : m_host(host)
        , m_timeoutMs(timeoutMs)
    {
        const QString h = host.contains(QLatin1Char(':')) ? QLatin1Char('[') + host + QLatin1Char(']') : host;
        m_hostHeader = QStringLiteral("%1:%2").arg(h).arg(kPort).toUtf8();
        m_to = QStringLiteral("http://%1/wsman").arg(QString::fromUtf8(m_hostHeader));
    }

    enum Mode { Raw, Sessions, SessionsAndEvents };

    SessionQuery run(const QString &user, const QString &domain, const QString &password, const QString &command,
                     Mode mode)
    {
        SessionQuery q;
        m_command = command;
        m_mode = mode;
        m_clock.start();
        if (!m_conn.open(m_host, qMin(m_timeoutMs, 4000), m_timeoutMs))
            return fail(q, SessionQuery::Unreachable, QCoreApplication::translate("WinRm", "Could not reach the server's remote management service (port 5985)."));
        if (!m_ntlm.init(m_host, user, domain, password))
            return fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "Could not set up NTLM."));
        if (!authenticate(q))
            return q;

        Reply r;
        const QString options = QStringLiteral("<w:OptionSet><w:Option Name=\"WINRS_NOPROFILE\">TRUE</w:Option>"
                                               "<w:Option Name=\"WINRS_CODEPAGE\">65001</w:Option></w:OptionSet>");
        const QString shell = QStringLiteral("<rsp:Shell><rsp:InputStreams>stdin</rsp:InputStreams>"
                                             "<rsp:OutputStreams>stdout stderr</rsp:OutputStreams></rsp:Shell>");
        if (!call(QString::fromLatin1(kTransferNs) + QStringLiteral("/Create"), {}, options, shell, r, q))
            return q;
        const QString shellId = r.shellId;
        if (shellId.isEmpty())
            return fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "The server did not open a shell."));

        bool ok = false;
        QByteArray out, err;
        const QString cmdOptions = QStringLiteral("<w:OptionSet><w:Option Name=\"WINRS_CONSOLEMODE_STDIN\">TRUE</w:Option>"
                                                  "<w:Option Name=\"WINRS_SKIP_CMD_SHELL\">FALSE</w:Option></w:OptionSet>");
        const QString commandLine = QStringLiteral("<rsp:CommandLine><rsp:Command>%1</rsp:Command></rsp:CommandLine>")
                                        .arg(xmlEscaped(m_command));
        if (call(QString::fromLatin1(kShellNs) + QStringLiteral("/Command"), shellId, cmdOptions, commandLine, r, q)) {
            const QString commandId = r.commandId;
            const QString receive = QStringLiteral("<rsp:Receive><rsp:DesiredStream CommandId=\"%1\">stdout stderr"
                                                   "</rsp:DesiredStream></rsp:Receive>").arg(commandId);
            while (m_clock.elapsed() < m_timeoutMs) {
                if (!call(QString::fromLatin1(kShellNs) + QStringLiteral("/Receive"), shellId, {}, receive, r, q,
                          true))
                    break;
                out += r.stdOut;
                err += r.stdErr;
                if (r.done) {
                    ok = true;
                    break;
                }
            }
            if (!ok && q.error.isEmpty())
                fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "The server did not answer in time."));
        }
        // Shells count against a per-user limit on the server.
        Reply ignored;
        SessionQuery ignoredQuery;
        call(QString::fromLatin1(kTransferNs) + QStringLiteral("/Delete"), shellId, {}, {}, ignored, ignoredQuery);

        if (!ok)
            return q;
        q.result = SessionQuery::Ok;
        if (m_mode == Raw) {
            q.output = out;
            if (!err.isEmpty())
                q.output += "\nRCSTDERR: " + err; // a command that fails to start only says so here
            return q;
        }
        if (m_mode == SessionsAndEvents) {
            const qsizetype split = out.indexOf(kEventsMarker);
            if (split >= 0) {
                q.events = parseEventLogs(out.left(split), &q.eventsSkipped);
                out.remove(0, split + kEventsMarker.size());
                out.remove(0, out.indexOf('\n') + 1); // rest of the echo line
            }
        }
        q.sessions = parseSessions(out, &q.serverNowUtcMs);
        return q;
    }

private:
    static SessionQuery &fail(SessionQuery &q, SessionQuery::Result result, const QString &error)
    {
        q.result = result;
        q.error = error;
        return q;
    }

    QByteArray request(const QByteArray &contentType, const QByteArray &body, const QByteArray &auth = {}) const
    {
        QByteArray r = "POST /wsman HTTP/1.1\r\nHost: " + m_hostHeader + "\r\nUser-Agent: RConnect\r\n"
                       "Connection: Keep-Alive\r\nContent-Type: " + contentType + "\r\n"
                       "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        if (!auth.isEmpty())
            r += "Authorization: Negotiate " + auth.toBase64() + "\r\n";
        return r + "\r\n" + body;
    }

    bool authenticate(SessionQuery &q)
    {
        const QByteArray contentType = "application/soap+xml;charset=UTF-8";
        QByteArray token;
        HttpResponse resp;
        if (!m_ntlm.step({}, token) || !m_conn.send(request(contentType, {}, token)) || !m_conn.receive(resp)) {
            fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "The server's remote management service did not answer."));
            return false;
        }
        QByteArray challenge;
        for (const QByteArray &v : std::as_const(resp.authenticate))
            if (v.startsWith("Negotiate "))
                challenge = QByteArray::fromBase64(v.mid(10));
        if (resp.status != 401 || challenge.isEmpty()) {
            fail(q, SessionQuery::Failed,
                 QCoreApplication::translate("WinRm", "The server does not accept this kind of sign-in (code %1).").arg(resp.status));
            return false;
        }
        if (!m_ntlm.step(challenge, token) || !m_conn.send(request(contentType, {}, token))
            || !m_conn.receive(resp)) {
            fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "The server's remote management service did not answer."));
            return false;
        }
        if (resp.status == 401) {
            fail(q, SessionQuery::AuthFailed,
                 QCoreApplication::translate("WinRm", "Sign-in was refused. Use one of the server's administrator accounts."));
            return false;
        }
        return true;
    }

    QByteArray envelope(const QString &action, const QString &shellId, const QString &options,
                        const QString &body) const
    {
        QString selector;
        if (!shellId.isEmpty())
            selector = QStringLiteral("<w:SelectorSet><w:Selector Name=\"ShellId\">%1</w:Selector></w:SelectorSet>")
                           .arg(xmlEscaped(shellId));
        const QString xml = QStringLiteral(
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
            "xmlns:w=\"http://schemas.dmtf.org/wbem/wsman/1/wsman.xsd\" "
            "xmlns:p=\"http://schemas.microsoft.com/wbem/wsman/1/wsman.xsd\" "
            "xmlns:rsp=\"http://schemas.microsoft.com/wbem/wsman/1/windows/shell\">"
            "<s:Header>"
            "<a:To>%1</a:To>"
            "<a:ReplyTo><a:Address s:mustUnderstand=\"true\">"
            "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:Address></a:ReplyTo>"
            "<w:MaxEnvelopeSize s:mustUnderstand=\"true\">153600</w:MaxEnvelopeSize>"
            "<a:MessageID>uuid:%2</a:MessageID>"
            "<w:Locale xml:lang=\"en-US\" s:mustUnderstand=\"false\"/>"
            "<p:DataLocale xml:lang=\"en-US\" s:mustUnderstand=\"false\"/>"
            "<w:OperationTimeout>PT%8S</w:OperationTimeout>"
            "<w:ResourceURI s:mustUnderstand=\"true\">%3</w:ResourceURI>"
            "<a:Action s:mustUnderstand=\"true\">%4</a:Action>"
            "%5%6"
            "</s:Header>"
            "<s:Body>%7</s:Body>"
            "</s:Envelope>")
            .arg(xmlEscaped(m_to), QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper(),
                 QString::fromLatin1(kShellUri), action, selector, options, body, QString::number(qMax(3, m_timeoutMs / 1000 - 1)));
        return xml.toUtf8();
    }

    bool call(const QString &action, const QString &shellId, const QString &options, const QString &body,
              Reply &reply, SessionQuery &q, bool timeoutIsOk = false)
    {
        const QByteArray plain = envelope(action, shellId, options, body);
        QByteArray sealed;
        if (!m_ntlm.seal(plain, sealed)) {
            fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "Could not encrypt the request."));
            return false;
        }
        const QByteArray payload = kBoundary + "\r\n\tContent-Type: " + kProtocol + "\r\n"
                                   "\tOriginalContent: type=application/soap+xml;charset=UTF-8;Length="
                                   + QByteArray::number(plain.size()) + "\r\n" + kBoundary + "\r\n" + kOctetHeader
                                   + sealed + kBoundary + "--\r\n";
        const QByteArray contentType = "multipart/encrypted;protocol=\"" + kProtocol
                                       + "\";boundary=\"Encrypted Boundary\"";
        HttpResponse resp;
        if (!m_conn.send(request(contentType, payload)) || !m_conn.receive(resp)) {
            fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "The connection to the server's remote management service was lost."));
            return false;
        }
        if (resp.status == 401) {
            fail(q, SessionQuery::AuthFailed,
                 QCoreApplication::translate("WinRm", "Sign-in was refused. Use one of the server's administrator accounts."));
            return false;
        }

        QByteArray xml = resp.body;
        if (resp.contentType.contains("multipart/encrypted")) {
            const qsizetype start = resp.body.indexOf(kOctetHeader);
            const qsizetype end = resp.body.lastIndexOf(kBoundary + "--");
            if (start < 0 || end < start || !m_ntlm.unseal(resp.body.mid(start + kOctetHeader.size(),
                                                                          end - start - kOctetHeader.size()), xml)) {
                fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "Could not decrypt the reply."));
                return false;
            }
        }
        reply = parseReply(xml);
        if (resp.status == 200)
            return true;
        if (timeoutIsOk && reply.faultCode == QLatin1String(kTimedOutCode)) {
            reply.stdOut.clear();
            return true;
        }
        fail(q, SessionQuery::Failed,
             reply.faultText.isEmpty() ? QCoreApplication::translate("WinRm", "The server returned an error (code %1).").arg(resp.status) : reply.faultText);
        return false;
    }

public:
    // Parses the event query output: "##RCLOG:<source>##" followed by that log's wevtutil XML.
    // Each event is parsed on its own, since failed sign-ins carry attacker-chosen names and one bad
    // event must not hide the rest. `skipped` counts unreadable events.
    static QList<AccessEvent> parseEventLogs(const QByteArray &output, int *skipped = nullptr)
    {
        QList<AccessEvent> events;
        qsizetype pos = output.indexOf(kLogMarker);
        while (pos >= 0) {
            const qsizetype next = output.indexOf(kLogMarker, pos + kLogMarker.size());
            const qsizetype start = output.indexOf('\n', pos);
            if (start >= 0 && (next < 0 || start < next))
                events << parseEvents(output.mid(start, next < 0 ? -1 : next - start), skipped);
            pos = next;
        }
        return events;
    }

private:
    // `<Event>...</Event><Event>...` with no root.
    static QList<AccessEvent> parseEvents(const QByteArray &output, int *skipped)
    {
        // Invalid UTF-8 and characters XML forbids become U+FFFD.
        QString text = QString::fromUtf8(output);
        for (QChar &c : text) {
            const ushort u = c.unicode();
            if ((u < 0x20 && u != 0x09 && u != 0x0A && u != 0x0D) || u == 0xFFFE || u == 0xFFFF)
                c = QChar::ReplacementCharacter;
        }
        QList<AccessEvent> events;
        qsizetype pos = 0;
        for (;;) {
            qsizetype begin = text.indexOf(QLatin1String("<Event"), pos);
            // "<Event " or "<Event>", not <EventData> and the like.
            while (begin >= 0 && begin + 6 < text.size() && text.at(begin + 6) != QLatin1Char(' ')
                   && text.at(begin + 6) != QLatin1Char('>'))
                begin = text.indexOf(QLatin1String("<Event"), begin + 6);
            if (begin < 0)
                break;
            const qsizetype end = text.indexOf(QLatin1String("</Event>"), begin);
            if (end < 0)
                break;
            pos = end + 8;
            AccessEvent e;
            const int outcome = parseEvent(QStringView(text).mid(begin, pos - begin), e);
            if (outcome > 0)
                events << e;
            else if (outcome < 0 && skipped)
                ++*skipped;
        }
        return events;
    }

    // 1: keep, 0: not logged, -1: unreadable.
    static int parseEvent(QStringView xml, AccessEvent &e)
    {
        QXmlStreamReader x(xml.toString());
        int eventId = 0;
        QString time;
        qint64 record = 0;
        QHash<QString, QString> data; // <Data Name="..."> of EventData, and the fields of UserData
        while (!x.atEnd()) {
            if (x.readNext() != QXmlStreamReader::StartElement)
                continue;
            const QStringView name = x.name();
            if (name == u"EventID") {
                eventId = x.readElementText().toInt();
            } else if (name == u"TimeCreated") {
                time = normalizedTime(x.attributes().value(u"SystemTime").toString());
            } else if (name == u"EventRecordID") {
                record = x.readElementText().toLongLong();
            } else if (name == u"Data") {
                const QString key = x.attributes().value(u"Name").toString();
                const QString value = x.readElementText().trimmed();
                if (!key.isEmpty())
                    data.insert(key, value);
            } else if (name == u"User" || name == u"SessionID" || name == u"Address" || name == u"Param1"
                       || name == u"Param2" || name == u"Param3" || name == u"SubjectUserName"
                       || name == u"SubjectDomainName" || name == u"Channel") {
                data.insert(name.toString(), x.readElementText().trimmed());
            }
        }
        if (x.hasError())
            return -1;
        return toAccessEvent(eventId, time, record, data, e) ? 1 : 0;
    }

    static QString accountName(const QString &domain, const QString &user)
    {
        return domain.isEmpty() || domain == QLatin1String("-") ? user : domain + QLatin1Char('\\') + user;
    }

    static QString dashToEmpty(const QString &value)
    {
        return value == QLatin1String("-") ? QString() : value;
    }

    // The account that did something (the Subject fields of the security events).
    static QString actor(const QHash<QString, QString> &d)
    {
        return accountName(d.value(QStringLiteral("SubjectDomainName")), d.value(QStringLiteral("SubjectUserName")));
    }

    // What a scheduled task runs, from its XML definition.
    static QString taskCommand(const QString &xml)
    {
        static const QRegularExpression command(QStringLiteral("<Command>(.*?)</Command>"),
                                                QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression arguments(QStringLiteral("<Arguments>(.*?)</Arguments>"),
                                                  QRegularExpression::DotMatchesEverythingOption);
        const QString run = (command.match(xml).captured(1) + QLatin1Char(' ') + arguments.match(xml).captured(1)).simplified();
        return run.left(300);
    }

    // NTSTATUS of a failed logon (4625), in words.
    static QString failureReason(const QString &status, const QString &subStatus)
    {
        static const QHash<QString, QString> reasons = {
            {QStringLiteral("0xc0000064"), QStringLiteral("unknown user name")},
            {QStringLiteral("0xc000006a"), QStringLiteral("wrong password")},
            {QStringLiteral("0xc000006d"), QStringLiteral("logon failed")},
            {QStringLiteral("0xc000006e"), QStringLiteral("account restriction")},
            {QStringLiteral("0xc000006f"), QStringLiteral("outside allowed hours")},
            {QStringLiteral("0xc0000070"), QStringLiteral("workstation restriction")},
            {QStringLiteral("0xc0000071"), QStringLiteral("password expired")},
            {QStringLiteral("0xc0000072"), QStringLiteral("account disabled")},
            {QStringLiteral("0xc0000133"), QStringLiteral("clocks out of sync")},
            {QStringLiteral("0xc000015b"), QStringLiteral("logon type not allowed")},
            {QStringLiteral("0xc0000193"), QStringLiteral("account expired")},
            {QStringLiteral("0xc0000234"), QStringLiteral("account locked out")},
        };
        const QString sub = subStatus.toLower();
        const QString key = sub.isEmpty() || sub == QLatin1String("0x0") ? status.toLower() : sub;
        return reasons.value(key, key);
    }

    static bool toAccessEvent(int id, const QString &time, qint64 record, const QHash<QString, QString> &d,
                              AccessEvent &e)
    {
        if (time.isEmpty())
            return false;
        e = {};
        e.timeUtc = time;
        e.recordId = record;
        e.kind = AccessEvent::Kind(id);
        switch (id) {
        case AccessEvent::Logon:
        case AccessEvent::Logoff:
        case AccessEvent::Disconnect:
        case AccessEvent::Reconnect:
            e.source = QStringLiteral("LSM");
            e.user = d.value(QStringLiteral("User"));
            e.address = d.value(QStringLiteral("Address"));
            e.sessionId = d.value(QStringLiteral("SessionID")).toInt();
            return true;
        case AccessEvent::Authenticated: // Param1 user, Param2 domain, Param3 client address
            e.source = QStringLiteral("RCM");
            e.user = accountName(d.value(QStringLiteral("Param2")), d.value(QStringLiteral("Param1")));
            e.address = d.value(QStringLiteral("Param3"));
            return true;
        case AccessEvent::SecurityLogon:
        case AccessEvent::FailedLogon:
            e.source = QStringLiteral("SEC");
            e.user = accountName(d.value(QStringLiteral("TargetDomainName")), d.value(QStringLiteral("TargetUserName")));
            e.address = dashToEmpty(d.value(QStringLiteral("IpAddress")));
            e.client = dashToEmpty(d.value(QStringLiteral("WorkstationName")));
            e.detail = QStringLiteral("logon type %1").arg(d.value(QStringLiteral("LogonType")));
            if (id == AccessEvent::FailedLogon)
                e.detail += QStringLiteral(": ")
                          + failureReason(d.value(QStringLiteral("Status")), d.value(QStringLiteral("SubStatus")));
            return true;
        case AccessEvent::SecurityReconnect:
        case AccessEvent::SecurityDisconnect:
            e.source = QStringLiteral("SEC");
            e.user = accountName(d.value(QStringLiteral("AccountDomain")), d.value(QStringLiteral("AccountName")));
            e.address = d.value(QStringLiteral("ClientAddress"));
            e.client = d.value(QStringLiteral("ClientName"));
            e.detail = d.value(QStringLiteral("SessionName"));
            return true;
        case AccessEvent::UserCreated: // the new account; detail: who created it
            e.source = QStringLiteral("SEC");
            e.user = accountName(d.value(QStringLiteral("TargetDomainName")), d.value(QStringLiteral("TargetUserName")));
            e.detail = actor(d);
            return true;
        case AccessEvent::GroupMemberAdded: { // the new member; detail: group|who added it
            e.source = QStringLiteral("SEC");
            const QString name = dashToEmpty(d.value(QStringLiteral("MemberName")));
            e.user = name.isEmpty() ? d.value(QStringLiteral("MemberSid")) : name;
            e.detail = d.value(QStringLiteral("TargetUserName")) + QLatin1Char('|') + actor(d);
            return true;
        }
        case AccessEvent::TaskCreated: // detail: task name: what it runs
            e.source = QStringLiteral("SEC");
            e.user = actor(d);
            e.detail = d.value(QStringLiteral("TaskName")) + QStringLiteral(": ") + taskCommand(d.value(QStringLiteral("TaskContent")));
            return true;
        case AccessEvent::LogCleared:
        case AccessEvent::SystemLogCleared: // the one who cleared it; detail: which log
            e.source = id == AccessEvent::LogCleared ? QStringLiteral("SEC") : QStringLiteral("SYS");
            e.user = actor(d);
            e.detail = id == AccessEvent::LogCleared ? QStringLiteral("Security") : d.value(QStringLiteral("Channel"), QStringLiteral("System"));
            return true;
        case AccessEvent::ServiceInstalled: // detail: service name: program
            e.source = QStringLiteral("SYS");
            e.user = d.value(QStringLiteral("AccountName"));
            e.detail = d.value(QStringLiteral("ServiceName")) + QStringLiteral(": ") + d.value(QStringLiteral("ImagePath"));
            return true;
        default:
            return false;
        }
    }

    // Always 7 fraction digits, so times compare as text.
    static QString normalizedTime(const QString &time)
    {
        const qsizetype dot = time.indexOf(QLatin1Char('.'));
        if (dot < 0)
            return time.left(time.size() - 1) + QStringLiteral(".0000000Z");
        QString fraction = time.mid(dot + 1, time.size() - dot - 2).left(7);
        return time.left(dot + 1) + fraction.leftJustified(7, QLatin1Char('0')) + QLatin1Char('Z');
    }

    // `quser` columns: USERNAME [SESSIONNAME] ID STATE IDLE LOGON-DATE LOGON-TIME. Headers and the
    // state are localized, so only the layout is used: disconnected sessions have no session name.
    static QList<RemoteSession> parseSessions(const QByteArray &output, qint64 *serverNowMs = nullptr)
    {
        qint64 unused = 0;
        qint64 &serverNow = serverNowMs ? *serverNowMs : unused;
        const qsizetype marker = output.indexOf(kMarker);
        QString users = QString::fromUtf8(marker < 0 ? output : output.left(marker));
        QString starts;
        const qsizetype startMarker = users.indexOf(QLatin1String("##RCSTART##"));
        if (startMarker >= 0) {
            starts = users.mid(startMarker + 11);
            users.truncate(startMarker);
        }
        const QString clients = marker < 0 ? QString() : QString::fromUtf8(output.mid(marker + kMarker.size()));

        static const QRegularExpression space(QStringLiteral("\\s+"));
        static const QRegularExpression date(QStringLiteral("^\\d{1,4}[./-]\\d{1,2}[./-]\\d{1,4}$"));
        static const QRegularExpression digits(QStringLiteral("^\\d+$"));

        QList<RemoteSession> sessions;
        const QStringList lines = users.split(QLatin1Char('\n'));
        for (qsizetype i = 1; i < lines.size(); ++i) { // line 0 is the header
            QStringList t = lines.at(i).trimmed().split(space, Qt::SkipEmptyParts);
            if (t.size() < 4)
                continue;
            RemoteSession s;
            s.user = t.at(0).startsWith(QLatin1Char('>')) ? t.at(0).mid(1) : t.at(0);
            s.connected = !digits.match(t.at(1)).hasMatch();
            const int idIndex = s.connected ? 2 : 1;
            if (!digits.match(t.value(idIndex)).hasMatch())
                continue;
            s.id = t.at(idIndex).toInt();
            qsizetype dateIndex = -1;
            for (qsizetype k = idIndex + 2; k < t.size() && dateIndex < 0; ++k)
                if (date.match(t.at(k)).hasMatch())
                    dateIndex = k;
            if (dateIndex > idIndex + 1)
                s.idleMinutes = parseIdle(t.at(dateIndex - 1));
            sessions << s;
        }

        // "NOW <time>" is the server clock, so samples and events agree.
        // Then "<session id> <time>" per shell; the first is the sign-in.
        for (const QString &raw : starts.split(QLatin1Char('\n'))) {
            const QStringList parts = raw.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (parts.size() != 2)
                continue;
            if (parts.at(0) == QLatin1String("NOW")) {
                const QDateTime now = QDateTime::fromString(parts.at(1).left(23) + QLatin1Char('Z'), Qt::ISODateWithMs);
                if (now.isValid())
                    serverNow = now.toMSecsSinceEpoch();
                continue;
            }
            const int id = parts.at(0).toInt();
            const QDateTime t = QDateTime::fromString(parts.at(1).left(23) + QLatin1Char('Z'), Qt::ISODateWithMs);
            if (!t.isValid())
                continue;
            for (RemoteSession &s : sessions)
                if (s.id == id && (s.logonUtcMs == 0 || t.toMSecsSinceEpoch() < s.logonUtcMs))
                    s.logonUtcMs = t.toMSecsSinceEpoch();
        }

        // HKEY_USERS\<sid>\Volatile Environment\<session id>
        //     CLIENTNAME    REG_SZ    PC-NAME
        int sessionId = -1;
        for (const QString &raw : clients.split(QLatin1Char('\n'))) {
            const QString line = raw.trimmed();
            if (line.startsWith(QLatin1String("HKEY_USERS\\"), Qt::CaseInsensitive)) {
                bool ok = false;
                sessionId = line.section(QLatin1Char('\\'), -1).toInt(&ok);
                if (!ok)
                    sessionId = -1;
                continue;
            }
            const qsizetype type = line.indexOf(QLatin1String("REG_SZ"));
            if (sessionId < 0 || !line.startsWith(QLatin1String("CLIENTNAME"), Qt::CaseInsensitive) || type < 0)
                continue;
            const QString client = line.mid(type + 6).trimmed();
            for (RemoteSession &s : sessions)
                if (s.id == sessionId)
                    s.client = client;
        }
        return sessions;
    }

    // ".", "none": under a minute; "5"; "1:05"; "2+03:04" (days+hours:minutes)
    static int parseIdle(const QString &text)
    {
        int days = 0;
        QString rest = text;
        const qsizetype plus = rest.indexOf(QLatin1Char('+'));
        if (plus >= 0) {
            days = rest.left(plus).toInt();
            rest = rest.mid(plus + 1);
        }
        const qsizetype colon = rest.indexOf(QLatin1Char(':'));
        const int minutes = colon >= 0 ? rest.left(colon).toInt() * 60 + rest.mid(colon + 1).toInt() : rest.toInt();
        return days * 1440 + minutes;
    }

    QString m_host;
    QString m_command;
    Mode m_mode = Sessions;
    int m_timeoutMs;
    QByteArray m_hostHeader;
    QString m_to;
    QElapsedTimer m_clock;
    Connection m_conn;
    Ntlm m_ntlm;
};

} // namespace

const QStringList &WinRm::eventSources()
{
    static const QStringList sources = {QStringLiteral("LSM"), QStringLiteral("RCM"), QStringLiteral("SEC"),
                                        QStringLiteral("SYS")};
    return sources;
}

namespace {

// A lost connection or a hiccup of the server's remote management service is tried once more, at once: one dropped
// packet must not show as "could not read who is connected" and clear the list of sessions. Only fast failures are
// repeated (a slow one already used the time), and a refused sign-in never is: repeated wrong passwords lock accounts.
template <typename Attempt>
SessionQuery withOneRetry(int timeoutMs, Attempt attempt)
{
    QElapsedTimer clock;
    clock.start();
    SessionQuery q = attempt();
    const bool transient = q.result == SessionQuery::Unreachable || q.result == SessionQuery::Failed;
    if (!transient || clock.elapsed() > timeoutMs / 3)
        return q;
    QThread::msleep(400);
    return attempt();
}

} // namespace

SessionQuery WinRm::querySessions(const QString &host, const QString &user, const QString &domain,
                                  const QString &password, int timeoutMs,
                                  const QHash<QString, EventCursor> *cursors)
{
    return withOneRetry(timeoutMs, [&] {
        return Client(host.trimmed(), timeoutMs)
            .run(user, domain, password, sessionCommand(cursors),
                 cursors ? Client::SessionsAndEvents : Client::Sessions);
    });
}

SessionQuery WinRm::runCommand(const QString &host, const QString &user, const QString &domain,
                               const QString &password, int timeoutMs, const QString &command)
{
    // A command may change something on the server (messages, auditing): it is not repeated.
    return Client(host.trimmed(), timeoutMs).run(user, domain, password, command, Client::Raw);
}
