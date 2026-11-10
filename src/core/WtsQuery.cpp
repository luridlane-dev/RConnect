#include "WtsQuery.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>

#include <mutex>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winnetwk.h>
#include <wtsapi32.h>

namespace {

constexpr int kSmbPort = 445;
constexpr qint64 kRetryAfterFailureMs = 5 * 60 * 1000;

std::mutex g_mutex;
QHash<QString, qint64> g_failedAt; // host (lower case) -> when it last failed

// Can the file-sharing port be reached? Without it the calls below would wait for a long time.
bool portOpen(const QString &host, int timeoutMs)
{
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW *results = nullptr;
    if (GetAddrInfoW(reinterpret_cast<const wchar_t *>(host.utf16()), std::to_wstring(kSmbPort).c_str(), &hints,
                     &results) != 0)
        return false;
    bool open = false;
    for (ADDRINFOW *a = results; a && !open; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        if (connect(s, a->ai_addr, int(a->ai_addrlen)) == 0) {
            open = true;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(s, &writable);
            timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            open = select(0, nullptr, &writable, nullptr, &tv) > 0 && FD_ISSET(s, &writable);
        }
        closesocket(s);
    }
    FreeAddrInfoW(results);
    return open;
}

// Signs in to the server's IPC$ share with the account, for the lifetime of this object. Windows then uses that
// account for the calls that follow. When this PC is already signed in to the server with another account, that
// one is used and nothing is changed.
class ServerLogon {
public:
    ServerLogon(const QString &host, const QString &user, const QString &domain, const QString &password)
    {
        if (user.isEmpty())
            return;
        m_path = QStringLiteral("\\\\") + host + QStringLiteral("\\IPC$");
        const QString account = domain.isEmpty() ? user : domain + QLatin1Char('\\') + user;
        std::wstring path = m_path.toStdWString();
        std::wstring name = account.toStdWString();
        std::wstring pw = password.toStdWString();
        NETRESOURCEW nr{};
        nr.dwType = RESOURCETYPE_ANY;
        nr.lpRemoteName = path.data();
        m_error = WNetAddConnection2W(&nr, pw.c_str(), name.c_str(), 0);
        SecureZeroMemory(pw.data(), pw.size() * sizeof(wchar_t));
        m_added = m_error == NO_ERROR;
        if (m_error == ERROR_SESSION_CREDENTIAL_CONFLICT || m_error == ERROR_ALREADY_ASSIGNED)
            m_error = NO_ERROR;
    }

    ~ServerLogon()
    {
        if (m_added) {
            std::wstring path = m_path.toStdWString();
            WNetCancelConnection2W(path.c_str(), 0, TRUE);
        }
    }

    ServerLogon(const ServerLogon &) = delete;
    ServerLogon &operator=(const ServerLogon &) = delete;

    DWORD error() const { return m_error; }

private:
    QString m_path;
    DWORD m_error = NO_ERROR;
    bool m_added = false;
};

qint64 fileTimeMs(const LARGE_INTEGER &t)
{
    constexpr qint64 kEpochDiff = 11644473600000LL; // 1601 to 1970, in ms
    return t.QuadPart <= 0 ? 0 : t.QuadPart / 10000 - kEpochDiff;
}

QString fromWide(const wchar_t *text)
{
    return text ? QString::fromWCharArray(text) : QString();
}

// One string of a session, or empty.
QString sessionText(HANDLE server, DWORD id, WTS_INFO_CLASS what)
{
    wchar_t *buffer = nullptr;
    DWORD bytes = 0;
    QString text;
    if (WTSQuerySessionInformationW(server, id, what, &buffer, &bytes) && buffer)
        text = fromWide(buffer);
    if (buffer)
        WTSFreeMemory(buffer);
    return text;
}

QString windowsError(DWORD code)
{
    wchar_t *message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    QString text = message ? fromWide(message).trimmed() : QString();
    if (message)
        LocalFree(message);
    return text.isEmpty() ? QStringLiteral("0x%1").arg(code, 0, 16) : text;
}

} // namespace

namespace WtsQuery {

SessionQuery sessions(const QString &host, const QString &user, const QString &domain, const QString &password,
                      int timeoutMs)
{
    SessionQuery q;
    q.result = SessionQuery::Failed;
    const QString key = host.toLower();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_failedAt.constFind(key);
        if (it != g_failedAt.constEnd() && now - it.value() < kRetryAfterFailureMs) {
            q.error = QCoreApplication::translate("WtsQuery", "The server does not answer Windows' session query either.");
            return q;
        }
    }
    const auto failed = [&](const QString &error) {
        std::lock_guard lock(g_mutex);
        g_failedAt.insert(key, QDateTime::currentMSecsSinceEpoch());
        q.result = SessionQuery::Failed;
        q.error = error;
        return q;
    };

    if (!portOpen(host, qMin(timeoutMs, 2500)))
        return failed(QCoreApplication::translate("WtsQuery", "The file-sharing port (445) of the server is closed."));

    const ServerLogon logon(host, user, domain, password);
    if (logon.error() != NO_ERROR)
        return failed(QCoreApplication::translate("WtsQuery", "Windows refused the sign-in: %1")
                          .arg(windowsError(logon.error())));

    HANDLE server = WTSOpenServerExW(const_cast<wchar_t *>(reinterpret_cast<const wchar_t *>(host.utf16())));
    if (!server)
        return failed(QCoreApplication::translate("WtsQuery", "Could not open the server: %1")
                          .arg(windowsError(GetLastError())));

    DWORD level = 1, count = 0;
    WTS_SESSION_INFO_1W *info = nullptr;
    if (!WTSEnumerateSessionsExW(server, &level, 0, &info, &count)) {
        const DWORD error = GetLastError();
        WTSCloseServer(server);
        return failed(QCoreApplication::translate("WtsQuery", "Could not list the sessions: %1").arg(windowsError(error)));
    }

    for (DWORD i = 0; i < count; ++i) {
        const WTS_SESSION_INFO_1W &s = info[i];
        const QString name = fromWide(s.pUserName);
        if (s.SessionId == 0 || name.isEmpty() || s.State == WTSListen)
            continue; // services, the listener, sessions waiting for a sign-in
        RemoteSession r;
        r.id = int(s.SessionId);
        r.user = name;
        r.connected = s.State == WTSActive;
        r.client = sessionText(server, s.SessionId, WTSClientName);

        WTSINFOW *details = nullptr;
        DWORD bytes = 0;
        if (WTSQuerySessionInformationW(server, s.SessionId, WTSSessionInfo, reinterpret_cast<LPWSTR *>(&details), &bytes)
            && details) {
            r.logonUtcMs = fileTimeMs(details->LogonTime);
            if (r.connected && details->LastInputTime.QuadPart > 0 && details->CurrentTime.QuadPart > 0)
                r.idleMinutes = int(qMax<qint64>(0, (details->CurrentTime.QuadPart - details->LastInputTime.QuadPart)
                                                        / 10000000 / 60));
            if (q.serverNowUtcMs == 0)
                q.serverNowUtcMs = fileTimeMs(details->CurrentTime);
        }
        if (details)
            WTSFreeMemory(details);
        q.sessions << r;
    }
    WTSFreeMemoryExW(WTSTypeSessionInfoLevel1, info, count);
    WTSCloseServer(server);

    {
        std::lock_guard lock(g_mutex);
        g_failedAt.remove(key);
    }
    q.result = SessionQuery::Ok;
    return q;
}

} // namespace WtsQuery
