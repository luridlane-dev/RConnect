#include "WinRm.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRegularExpression>
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
// Sessions, then the client name of each session from the users' volatile environment.
const char *const kCommand =
    "quser & echo ##RCONNECT## & for /f \"delims=\" %k in ('reg query HKU') do "
    "@reg query \"%k\\Volatile Environment\" /s /v CLIENTNAME 2>nul";

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

    SessionQuery run(const QString &user, const QString &domain, const QString &password)
    {
        SessionQuery q;
        m_clock.start();
        if (!m_conn.open(m_host, qMin(m_timeoutMs, 4000), m_timeoutMs))
            return fail(q, SessionQuery::Unreachable, QCoreApplication::translate("WinRm", "WinRM (port 5985) is not reachable."));
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
        QByteArray out;
        const QString cmdOptions = QStringLiteral("<w:OptionSet><w:Option Name=\"WINRS_CONSOLEMODE_STDIN\">TRUE</w:Option>"
                                                  "<w:Option Name=\"WINRS_SKIP_CMD_SHELL\">FALSE</w:Option></w:OptionSet>");
        const QString command = QStringLiteral("<rsp:CommandLine><rsp:Command>%1</rsp:Command></rsp:CommandLine>")
                                    .arg(xmlEscaped(QString::fromLatin1(kCommand)));
        if (call(QString::fromLatin1(kShellNs) + QStringLiteral("/Command"), shellId, cmdOptions, command, r, q)) {
            const QString commandId = r.commandId;
            const QString receive = QStringLiteral("<rsp:Receive><rsp:DesiredStream CommandId=\"%1\">stdout stderr"
                                                   "</rsp:DesiredStream></rsp:Receive>").arg(commandId);
            while (m_clock.elapsed() < m_timeoutMs) {
                if (!call(QString::fromLatin1(kShellNs) + QStringLiteral("/Receive"), shellId, {}, receive, r, q,
                          true))
                    break;
                out += r.stdOut;
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
        q.sessions = parseSessions(out);
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
            fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "WinRM did not answer."));
            return false;
        }
        QByteArray challenge;
        for (const QByteArray &v : std::as_const(resp.authenticate))
            if (v.startsWith("Negotiate "))
                challenge = QByteArray::fromBase64(v.mid(10));
        if (resp.status != 401 || challenge.isEmpty()) {
            fail(q, SessionQuery::Failed,
                 QCoreApplication::translate("WinRm", "WinRM does not accept NTLM sign-in (HTTP %1).").arg(resp.status));
            return false;
        }
        if (!m_ntlm.step(challenge, token) || !m_conn.send(request(contentType, {}, token))
            || !m_conn.receive(resp)) {
            fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "WinRM did not answer."));
            return false;
        }
        if (resp.status == 401) {
            fail(q, SessionQuery::AuthFailed,
                 QCoreApplication::translate("WinRm", "Sign-in was refused. WinRM needs an administrator account on the server."));
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
            fail(q, SessionQuery::Failed, QCoreApplication::translate("WinRm", "The WinRM connection was lost."));
            return false;
        }
        if (resp.status == 401) {
            fail(q, SessionQuery::AuthFailed,
                 QCoreApplication::translate("WinRm", "Sign-in was refused. WinRM needs an administrator account on the server."));
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
             reply.faultText.isEmpty() ? QCoreApplication::translate("WinRm", "WinRM error (HTTP %1).").arg(resp.status) : reply.faultText);
        return false;
    }

    // `quser` columns: USERNAME [SESSIONNAME] ID STATE IDLE LOGON-DATE LOGON-TIME. Headers and the
    // state are localized, so only the layout is used: disconnected sessions have no session name.
    static QList<RemoteSession> parseSessions(const QByteArray &output)
    {
        const qsizetype marker = output.indexOf(kMarker);
        const QString users = QString::fromUtf8(marker < 0 ? output : output.left(marker));
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
    int m_timeoutMs;
    QByteArray m_hostHeader;
    QString m_to;
    QElapsedTimer m_clock;
    Connection m_conn;
    Ntlm m_ntlm;
};

} // namespace

SessionQuery WinRm::querySessions(const QString &host, const QString &user, const QString &domain,
                                  const QString &password, int timeoutMs)
{
    return Client(host.trimmed(), timeoutMs).run(user, domain, password);
}
