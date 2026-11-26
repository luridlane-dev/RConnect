#include "WinRm.h"
#include "WindowsTcp.h"

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
const QByteArray kSessionListMarker = "##RCQUSEROK##";
const QByteArray kEventsMarker = "##RCONNECT-EVENTS##";
const QByteArray kConnectionsMarker = "##RCONNECT-CONNECTIONS##";
// Starts one log's output ("##RCLOG:LSM##"), so each log is parsed on its own.
const QByteArray kLogMarker = "##RCLOG:";
constexpr int kMaxEvents = 300; // per log and call
constexpr qint64 kMaxLookbackMs = 7LL * 24 * 3600 * 1000;

QString timeClause(const EventCursor &c)
{
    // Only a time in the exact format the server writes ends up in the command line.
    static const QRegularExpression format(
        QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d(\\.\\d{1,9})?Z$"));
    if (format.match(c.afterUtc).hasMatch()) {
        if (c.afterRecordId > 0)
            return QStringLiteral("(TimeCreated[@SystemTime>'%1'] or (TimeCreated[@SystemTime='%1'] and EventRecordID>%2))")
                .arg(c.afterUtc).arg(c.afterRecordId);
        return QStringLiteral("TimeCreated[@SystemTime>='%1']").arg(c.afterUtc);
    }
    const qint64 ms = c.lookbackMs > 0 ? qMin(c.lookbackMs, kMaxLookbackMs) : kMaxLookbackMs;
    return QStringLiteral("TimeCreated[timediff(@SystemTime)<=%1]").arg(ms);
}

QString eventLogQuery(const QString &source, const QString &log, QString xpath, const EventCursor &cursor)
{
    xpath.replace(QLatin1String("TIME"), timeClause(cursor));
    return QStringLiteral("echo %1%2## & (wevtutil qe %3 /c:%4 /rd:false /f:xml /q:\"%5\" 2>nul "
                          "&& (echo. & echo ##RCLOGOK:%2##)) & ")
        .arg(QString::fromLatin1(kLogMarker), source, log).arg(kMaxEvents).arg(xpath);
}

QString eventCommand(const QHash<QString, EventCursor> &cursors)
{
    const QString lsm = QStringLiteral("*[System[(EventID=21 or EventID=23 or EventID=24 or EventID=25) and TIME]]");
    const QString rcm = QStringLiteral("*[System[EventID=1149 and TIME]]");
    const QString sec = QStringLiteral(
        "*[System[TIME] and (System[EventID=4778 or EventID=4779] "
        "or (System[EventID=4624] and EventData[Data[@Name='LogonType']='10']) "
        "or (System[EventID=4625] and EventData[Data[@Name='LogonType']='10' or Data[@Name='LogonType']='3']) "
        "or System[EventID=4720 or EventID=4698 or EventID=1102] "
        "or (System[EventID=4732] and EventData[Data[@Name='TargetSid']='S-1-5-32-544' "
        "or Data[@Name='TargetSid']='S-1-5-32-555']))]");
    // Separate selectors stay under the Windows Event Log XPath complexity limit.
    // Legacy types keep their SEC cursor; the new types have their own backfill.
    const QString audit = QStringLiteral(
        "*[System[TIME] and (System[(EventID>=4722 and EventID<=4726) or (EventID>=4699 and EventID<=4702) "
        "or EventID=4719 or EventID=4740] "
        "or (System[EventID=4733] and EventData[Data[@Name='TargetSid']='S-1-5-32-544' "
        "or Data[@Name='TargetSid']='S-1-5-32-555']))]");
    const QString sys = QStringLiteral(
        "*[System[TIME and ((Provider[@Name='Service Control Manager'] and EventID=7045) "
        "or (Provider[@Name='Microsoft-Windows-Eventlog'] and EventID=104))]]");
    return eventLogQuery(QStringLiteral("LSM"),
                         QStringLiteral("Microsoft-Windows-TerminalServices-LocalSessionManager/Operational"), lsm,
                         cursors.value(QStringLiteral("LSM")))
         + eventLogQuery(QStringLiteral("RCM"),
                         QStringLiteral("Microsoft-Windows-TerminalServices-RemoteConnectionManager/Operational"), rcm,
                         cursors.value(QStringLiteral("RCM")))
         + eventLogQuery(QStringLiteral("SEC"), QStringLiteral("Security"), sec, cursors.value(QStringLiteral("SEC")))
         + eventLogQuery(QStringLiteral("AUD"), QStringLiteral("Security"), audit, cursors.value(QStringLiteral("AUD")))
         + eventLogQuery(QStringLiteral("SYS"), QStringLiteral("System"), sys, cursors.value(QStringLiteral("SYS")))
         + QStringLiteral("echo %1 & ").arg(QString::fromLatin1(kEventsMarker));
}

// Sessions, then the client name of each session from the users' volatile environment. The access
// events (when asked for) come first, because `for` takes the rest of the line as its body.
// Server UTC time is read from the WinRM HTTP Date response header rather than spawning PowerShell.
QString sessionCommand(const QHash<QString, EventCursor> *cursors, bool includeTimes)
{
    const QString connections = includeTimes
        ? QStringLiteral("echo ##RCLOG:LSM## & wevtutil qe Microsoft-Windows-TerminalServices-LocalSessionManager/Operational "
                         "/rd:true /c:256 /f:xml /q:\"*[System[(EventID=21 or EventID=23 or EventID=24 or EventID=25)]]\" "
                         "2>nul & echo %1 & ").arg(QString::fromLatin1(kConnectionsMarker))
        : QString();
    return (cursors ? eventCommand(*cursors) : QString())
         + connections
         + QStringLiteral("(quser && echo %1) & echo ##RCONNECT## & for /f \"delims=\" %k in ('reg query HKU') do "
                          "@reg query \"%k\\Volatile Environment\" /s /v CLIENTNAME 2>nul")
               .arg(QString::fromLatin1(kSessionListMarker));
}

struct HttpResponse {
    int status = 0;
    qint64 dateUtcMs = 0;
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
        const auto result = WindowsTcp::open(host, kPort, connectMs);
        m_socket = result.socket;
        m_error = result.error;
        if (m_socket == INVALID_SOCKET)
            return false;
        u_long blocking = 0;
        const DWORD ms = DWORD(ioMs);
        if (ioctlsocket(m_socket, FIONBIO, &blocking) == SOCKET_ERROR
            || setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof ms) == SOCKET_ERROR
            || setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms), sizeof ms) == SOCKET_ERROR) {
            m_error = WindowsTcp::systemError(WSAGetLastError());
            return false;
        }
        return true;
    }

    QString error() const { return m_error; }

    bool send(const QByteArray &data)
    {
        qsizetype sent = 0;
        while (sent < data.size()) {
            const int n = ::send(m_socket, data.constData() + sent, int(data.size() - sent), 0);
            if (n <= 0) {
                m_error = n == SOCKET_ERROR ? WindowsTcp::systemError(WSAGetLastError())
                    : QCoreApplication::translate("WinRm", "The socket sent no bytes.");
                return false;
            }
            sent += n;
        }
        return true;
    }

    bool receive(HttpResponse &r)
    {
        qsizetype end;
        while ((end = m_buffer.indexOf("\r\n\r\n", m_pos)) < 0)
            if (!fill())
                return false;
        const QList<QByteArray> lines = m_buffer.mid(m_pos, end - m_pos).split('\n');
        consume(end - m_pos + 4);

        const QList<QByteArray> statusLine = lines.value(0).trimmed().split(' ');
        r = {};
        r.status = statusLine.value(1).toInt();
        if (!statusLine.value(0).startsWith("HTTP/") || r.status < 100 || r.status > 599) {
            m_error = QCoreApplication::translate("WinRm", "The peer did not send a valid HTTP status line.");
            return false;
        }
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
            else if (name == "date") {
                const QDateTime dt = QDateTime::fromString(QString::fromLatin1(value), Qt::RFC2822Date);
                if (dt.isValid())
                    r.dateUtcMs = dt.toMSecsSinceEpoch();
            }
        }

        if (!chunked)
            return readInto(length, r.body);
        for (;;) {
            QByteArray sizeLine;
            if (!readLine(sizeLine))
                return false;
            const qsizetype semi = sizeLine.indexOf(';');
            const qsizetype size = (semi < 0 ? sizeLine : sizeLine.left(semi)).trimmed().toLongLong(nullptr, 16);
            QByteArray crlf;
            if (size == 0)
                return readLine(crlf);
            if (!readInto(size, r.body) || !readLine(crlf))
                return false;
        }
    }

private:
    void consume(qsizetype n)
    {
        m_pos += n;
        if (m_pos >= m_buffer.size()) {
            m_buffer.clear();
            m_pos = 0;
        } else if (m_pos >= 16384 && m_pos * 2 >= m_buffer.size()) {
            m_buffer.remove(0, m_pos);
            m_pos = 0;
        }
    }

    bool fill()
    {
        char buf[16384];
        const int n = recv(m_socket, buf, sizeof buf, 0);
        if (n <= 0) {
            m_error = n == SOCKET_ERROR ? WindowsTcp::systemError(WSAGetLastError())
                : QCoreApplication::translate("WinRm", "The peer closed the TCP connection.");
            return false;
        }
        m_buffer.append(buf, n);
        return true;
    }

    bool readInto(qsizetype size, QByteArray &out)
    {
        while (m_buffer.size() - m_pos < size)
            if (!fill())
                return false;
        out.append(m_buffer.constData() + m_pos, size);
        consume(size);
        return true;
    }

    bool readLine(QByteArray &out)
    {
        qsizetype end;
        while ((end = m_buffer.indexOf("\r\n", m_pos)) < 0)
            if (!fill())
                return false;
        out = m_buffer.mid(m_pos, end - m_pos);
        consume(end - m_pos + 2);
        return true;
    }

    SOCKET m_socket = INVALID_SOCKET;
    QByteArray m_buffer;
    qsizetype m_pos = 0;
    QString m_error;
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

    QString error() const { return WindowsTcp::systemError(DWORD(m_status)); }

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
        m_status = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t *>(L"NTLM"),
                                                             SECPKG_CRED_OUTBOUND, nullptr, &id, nullptr, nullptr,
                                                             &m_credentials, &expiry);
        SecureZeroMemory(password.data(), size_t(password.size()) * sizeof(QChar));
        m_hasCredentials = m_status == SEC_E_OK;
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
        m_status = InitializeSecurityContextW(
            &m_credentials, m_hasContext ? &m_context : nullptr, m_target.data(), flags, 0, SECURITY_NATIVE_DREP,
            m_hasContext ? &inDesc : nullptr, 0, &m_context, &outDesc, &attrs, &expiry);
        if (m_status != SEC_E_OK && m_status != SEC_I_CONTINUE_NEEDED)
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
        m_status = QueryContextAttributesW(&m_context, SECPKG_ATTR_SIZES, &sizes);
        if (m_status != SEC_E_OK)
            return false;
        QByteArray signature(qsizetype(sizes.cbSecurityTrailer), '\0');
        QByteArray data = plain;
        SecBuffer bufs[2] = {{ULONG(signature.size()), SECBUFFER_TOKEN, signature.data()},
                             {ULONG(data.size()), SECBUFFER_DATA, data.data()}};
        SecBufferDesc desc{SECBUFFER_VERSION, 2, bufs};
        m_status = EncryptMessage(&m_context, 0, &desc, 0);
        if (m_status != SEC_E_OK)
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
    SECURITY_STATUS m_status = SEC_E_OK;
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

void applyConnectionTimes(QList<RemoteSession> &sessions, const QList<AccessEvent> &events, qint64 serverNow)
{
    QHash<int, const AccessEvent *> latest;
    for (const AccessEvent &event : events) {
        const auto *previous = latest.value(event.sessionId);
        if (event.sessionId > 0 && (!previous || event.recordId > previous->recordId))
            latest.insert(event.sessionId, &event);
    }
    for (RemoteSession &session : sessions) {
        const auto *event = latest.value(session.id);
        if (!session.connected || !event
            || (event->kind != AccessEvent::Logon && event->kind != AccessEvent::Reconnect))
            continue;
        const QString user = session.user.contains(QLatin1Char('\\')) ? event->user : event->user.section(QLatin1Char('\\'), -1);
        if (user.isEmpty() || session.user.compare(user, Qt::CaseInsensitive) != 0)
            continue;
        const QDateTime time = QDateTime::fromString(event->timeUtc.left(23) + QLatin1Char('Z'), Qt::ISODateWithMs);
        if (time.isValid() && (serverNow <= 0 || time.toMSecsSinceEpoch() <= serverNow + 1000)) {
            session.connectedSinceUtcMs = time.toMSecsSinceEpoch();
            if (event->kind == AccessEvent::Logon)
                session.logonUtcMs = session.connectedSinceUtcMs;
        }
    }
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
                     Mode mode, const QStringList &arguments = {}, const QByteArray &standardInput = {})
    {
        SessionQuery q;
        m_mode = mode;
        m_clock.start();
        if (!m_conn.open(m_host, qMin(m_timeoutMs, 4000), m_timeoutMs))
            return fail(q, SessionQuery::Unreachable, QCoreApplication::translate("WinRm", "TCP connection failed · %1").arg(m_conn.error()));
        if (!m_ntlm.init(m_host, user, domain, password))
            return fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "NTLM setup failed · %1").arg(m_ntlm.error()));
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
        const QString cmdOptions = QStringLiteral("<w:OptionSet><w:Option Name=\"WINRS_CONSOLEMODE_STDIN\">%1</w:Option>"
                                                  "<w:Option Name=\"WINRS_SKIP_CMD_SHELL\">%2</w:Option></w:OptionSet>")
                                       .arg(standardInput.isEmpty() ? QStringLiteral("TRUE") : QStringLiteral("FALSE"),
                                            arguments.isEmpty() ? QStringLiteral("FALSE") : QStringLiteral("TRUE"));
        QString commandLine = QStringLiteral("<rsp:CommandLine><rsp:Command>%1</rsp:Command>")
                                  .arg(xmlEscaped(command));
        for (const QString &argument : arguments)
            commandLine += QStringLiteral("<rsp:Arguments>%1</rsp:Arguments>").arg(xmlEscaped(argument));
        commandLine += QStringLiteral("</rsp:CommandLine>");
        if (call(QString::fromLatin1(kShellNs) + QStringLiteral("/Command"), shellId, cmdOptions, commandLine, r, q)) {
            const QString commandId = xmlEscaped(r.commandId);
            bool canReceive = !commandId.isEmpty();
            if (canReceive && !standardInput.isEmpty()) {
                const QString input = QStringLiteral("<rsp:Send><rsp:Stream Name=\"stdin\" CommandId=\"%1\" "
                                                     "End=\"true\">%2</rsp:Stream></rsp:Send>")
                                          .arg(commandId, QString::fromLatin1(standardInput.toBase64()));
                canReceive = call(QString::fromLatin1(kShellNs) + QStringLiteral("/Send"), shellId, {}, input, r, q);
            }
            const QString receive = QStringLiteral("<rsp:Receive><rsp:DesiredStream CommandId=\"%1\">stdout stderr"
                                                   "</rsp:DesiredStream></rsp:Receive>").arg(commandId);
            while (canReceive && m_clock.elapsed() < m_timeoutMs) {
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
                parseEventPage(out.left(split), q);
                out.remove(0, split + kEventsMarker.size());
                out.remove(0, out.indexOf('\n') + 1); // rest of the echo line
            } else {
                parseEventPage({}, q);
            }
        }
        QList<AccessEvent> connectionEvents;
        const qsizetype split = out.indexOf(kConnectionsMarker);
        if (split >= 0) {
            connectionEvents = parseEventLogs(out.left(split));
            out.remove(0, out.indexOf('\n', split) + 1);
        }
        q.sessions = parseSessions(out, &q.serverNowUtcMs);
        if (q.serverNowUtcMs <= 0 && m_serverNowUtcMs > 0)
            q.serverNowUtcMs = m_serverNowUtcMs;
        q.sessionsRead = out.contains(kSessionListMarker) || !q.sessions.isEmpty();
        applyConnectionTimes(q.sessions, connectionEvents, q.serverNowUtcMs);
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
        const auto exchange = [&](const QByteArray &challenge) {
            if (!m_ntlm.step(challenge, token)) {
                fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "NTLM exchange failed · %1")
                                                  .arg(m_ntlm.error()));
                return false;
            }
            if (!m_conn.send(request(contentType, {}, token)) || !m_conn.receive(resp)) {
                fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "HTTP authentication exchange failed · %1")
                                                  .arg(m_conn.error()));
                return false;
            }
            return true;
        };
        if (!exchange({}))
            return false;
        QByteArray challenge;
        for (const QByteArray &v : std::as_const(resp.authenticate))
            if (v.startsWith("Negotiate "))
                challenge = QByteArray::fromBase64(v.mid(10));
        if (resp.status != 401 || challenge.isEmpty()) {
            fail(q, SessionQuery::Failed,
                 QCoreApplication::translate("WinRm", "HTTP %1 received during authentication without a usable Negotiate challenge.").arg(resp.status));
            return false;
        }
        if (!exchange(challenge))
            return false;
        if (resp.status == 401) {
            fail(q, SessionQuery::AuthFailed,
                 QCoreApplication::translate("WinRm", "Authentication rejected (HTTP 401)."));
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
            fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "Could not encrypt the request · %1").arg(m_ntlm.error()));
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
            fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "WS-Management %1 exchange failed · %2")
                                              .arg(action.section(QLatin1Char('/'), -1), m_conn.error()));
            return false;
        }
        if (resp.dateUtcMs > 0)
            m_serverNowUtcMs = resp.dateUtcMs;
        if (resp.status == 401) {
            fail(q, SessionQuery::AuthFailed,
                 QCoreApplication::translate("WinRm", "Authentication rejected (HTTP 401)."));
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
        QString error = QCoreApplication::translate("WinRm", "WS-Management %1 returned HTTP %2.")
                            .arg(action.section(QLatin1Char('/'), -1)).arg(resp.status);
        if (!reply.faultCode.isEmpty())
            error += QStringLiteral(" · WSManFault %1").arg(reply.faultCode);
        if (!reply.faultText.isEmpty())
            error += QStringLiteral(" · ") + reply.faultText.simplified().left(600);
        fail(q, SessionQuery::Failed, error);
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

    static void parseEventPage(const QByteArray &output, SessionQuery &q)
    {
        q.events = parseEventLogs(output, &q.eventsSkipped);
        QHash<QString, int> counts;
        for (const AccessEvent &event : std::as_const(q.events))
            q.moreEvents |= ++counts[event.source] == kMaxEvents;
        QSet<QByteArray> completed;
        qsizetype pos = output.indexOf("##RCLOGOK:");
        while (pos >= 0) {
            const qsizetype end = output.indexOf("##", pos + 10);
            if (end < 0)
                break;
            completed.insert(output.mid(pos, end - pos + 2));
            pos = output.indexOf("##RCLOGOK:", end + 2);
        }
        for (const QString &source : WinRm::eventSources())
            if (!completed.contains("##RCLOGOK:" + source.toLatin1() + "##"))
                q.failedEventSources.insert(source);
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
        QXmlStreamReader x(xml);
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
        case AccessEvent::UserCreated:
        case AccessEvent::UserEnabled:
        case AccessEvent::UserDisabled:
        case AccessEvent::UserDeleted:
        case AccessEvent::PasswordChangeAttempt:
        case AccessEvent::PasswordResetAttempt:
        case AccessEvent::AccountLocked:
            e.source = id == AccessEvent::UserCreated ? QStringLiteral("SEC") : QStringLiteral("AUD");
            e.user = accountName(d.value(QStringLiteral("TargetDomainName")), d.value(QStringLiteral("TargetUserName")));
            e.detail = actor(d);
            if (id == AccessEvent::AccountLocked)
                e.client = dashToEmpty(d.value(QStringLiteral("CallerComputerName")));
            return true;
        case AccessEvent::GroupMemberAdded:
        case AccessEvent::GroupMemberRemoved: {
            e.source = id == AccessEvent::GroupMemberAdded ? QStringLiteral("SEC") : QStringLiteral("AUD");
            const QString name = dashToEmpty(d.value(QStringLiteral("MemberName")));
            e.user = name.isEmpty() ? d.value(QStringLiteral("MemberSid")) : name;
            e.detail = d.value(QStringLiteral("TargetUserName")) + QLatin1Char('|') + actor(d);
            return true;
        }
        case AccessEvent::TaskCreated:
        case AccessEvent::TaskRemoved:
        case AccessEvent::TaskEnabled:
        case AccessEvent::TaskDisabled:
        case AccessEvent::TaskUpdated:
            e.source = id == AccessEvent::TaskCreated ? QStringLiteral("SEC") : QStringLiteral("AUD");
            e.user = actor(d);
            e.detail = d.value(QStringLiteral("TaskName"));
            if (const QString command = taskCommand(d.value(QStringLiteral("TaskContentNew"),
                                                            d.value(QStringLiteral("TaskContent")))); !command.isEmpty())
                e.detail += QStringLiteral(": ") + command;
            return true;
        case AccessEvent::AuditPolicyChanged: {
            e.source = QStringLiteral("AUD");
            e.user = actor(d);
            const QString guid = d.value(QStringLiteral("SubcategoryGuid"));
            const QString changes = d.value(QStringLiteral("AuditPolicyChanges")).simplified();
            e.detail = changes.isEmpty() ? guid : (guid + QLatin1Char('|') + changes);
            return true;
        }
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
        for (const QString &raw : starts.split(QLatin1Char('\n'))) {
            const QStringList parts = raw.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (parts.size() != 2 || parts.at(0) != QLatin1String("NOW"))
                continue;
            const QDateTime now = QDateTime::fromString(parts.at(1).left(23) + QLatin1Char('Z'), Qt::ISODateWithMs);
            if (now.isValid())
                serverNow = now.toMSecsSinceEpoch();
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

    // ".", "none" (and localized equivalents like Turkish "yok"): under a minute; "5"; "1:05"; "2+03:04" (days+hours:minutes)
    static int parseIdle(const QString &text)
    {
        if (text == QLatin1String("."))
            return 0;
        const QString lower = text.toLower();
        static const QStringList kZeroWords = {
            QStringLiteral("none"), QStringLiteral("yok"), QStringLiteral("keine"), QStringLiteral("aucun"),
            QStringLiteral("ninguno"), QStringLiteral("nenhum"), QStringLiteral("nessuno"), QStringLiteral("geen"),
            QStringLiteral("нет")
        };
        if (kZeroWords.contains(lower))
            return 0;
        static const QRegularExpression format(QStringLiteral("^(?:(\\d{1,6})\\+)?(?:(\\d{1,6}):)?(\\d{1,6})$"));
        const auto match = format.match(text);
        if (!match.hasMatch())
            return -1;
        const qint64 minutes = match.captured(1).toLongLong() * 1440
            + match.captured(2).toLongLong() * 60 + match.captured(3).toLongLong();
        // Guard against Windows' known epoch-overflow idle output (e.g. 24692+13:29 when LastInputTime == 0).
        if (minutes < 0 || minutes > 365LL * 1440)
            return -1;
        return int(minutes);
    }

    QString m_host;
    Mode m_mode = Sessions;
    int m_timeoutMs;
    qint64 m_serverNowUtcMs = 0;
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
                                        QStringLiteral("AUD"), QStringLiteral("SYS")};
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
                                  const QHash<QString, EventCursor> *cursors, bool includeTimes)
{
    const QString command = sessionCommand(cursors, includeTimes);
    return withOneRetry(timeoutMs, [&] {
        return Client(host.trimmed(), timeoutMs)
            .run(user, domain, password, command,
                 cursors ? Client::SessionsAndEvents : Client::Sessions);
    });
}

SessionQuery WinRm::runCommand(const QString &host, const QString &user, const QString &domain,
                               const QString &password, int timeoutMs, const QString &command)
{
    // A command may change something on the server (messages, auditing): it is not repeated.
    return Client(host.trimmed(), timeoutMs).run(user, domain, password, command, Client::Raw);
}

SessionQuery WinRm::runPowerShell(const QString &host, const QString &user, const QString &domain,
                                 const QString &password, int timeoutMs, const QString &script)
{
    // WinRS can limit a command line even when cmd.exe is skipped. Keep the launcher fixed and send the script on stdin.
    const QString launcher = QStringLiteral("$stdin=[IO.StreamReader]::new([Console]::OpenStandardInput(),[Text.Encoding]::UTF8);"
                                            "& ([ScriptBlock]::Create($stdin.ReadToEnd()))");
    const QByteArray encoded = QByteArray(reinterpret_cast<const char *>(launcher.utf16()), launcher.size() * 2).toBase64();
    return Client(host.trimmed(), timeoutMs)
        .run(user, domain, password, QStringLiteral("powershell.exe"), Client::Raw,
             {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
              QStringLiteral("-EncodedCommand"), QString::fromLatin1(encoded)},
             script.toUtf8());
}
