#pragma once

#include <QCoreApplication>
#include <QStringList>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace WindowsTcp {

inline QString systemError(DWORD code)
{
    wchar_t *message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    const QString text = message ? QString::fromWCharArray(message).simplified() : QString();
    if (message)
        LocalFree(message);
    const QString number = QStringLiteral("%1 (0x%2)").arg(code).arg(code, 0, 16);
    return text.isEmpty() ? number : number + QStringLiteral(" · ") + text;
}

struct Result {
    SOCKET socket = INVALID_SOCKET; // non-blocking on success; owned by the caller
    QString error;
};

inline QString connectAddress(SOCKET socket, const ADDRINFOW &address, int timeoutMs)
{
    if (::connect(socket, address.ai_addr, int(address.ai_addrlen)) == 0)
        return {};
    const int startError = WSAGetLastError();
    if (startError != WSAEWOULDBLOCK)
        return systemError(startError);

    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(socket, &writable);
    FD_SET(socket, &failed);
    timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    const int ready = select(0, nullptr, &writable, &failed, &timeout);
    if (ready == 0)
        return QCoreApplication::translate("WindowsTcp", "No TCP response within the local %1 ms deadline.").arg(timeoutMs);
    if (ready == SOCKET_ERROR)
        return systemError(WSAGetLastError());

    int status = 0;
    int size = sizeof(status);
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&status), &size) == SOCKET_ERROR)
        return systemError(WSAGetLastError());
    if (status != 0)
        return systemError(status);
    if (FD_ISSET(socket, &writable) && !FD_ISSET(socket, &failed))
        return {};
    return QCoreApplication::translate("WindowsTcp", "The socket reported a connection failure without an error code.");
}

inline Result open(const QString &host, quint16 port, int timeoutMs)
{
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    PADDRINFOW addresses = nullptr;
    const QString service = QString::number(port);
    const int resolved = GetAddrInfoW(reinterpret_cast<PCWSTR>(host.utf16()),
                                      reinterpret_cast<PCWSTR>(service.utf16()), &hints, &addresses);
    if (resolved != 0)
        return {INVALID_SOCKET, QCoreApplication::translate("WindowsTcp", "Address resolution failed · %1")
                                    .arg(systemError(resolved))};

    Result result;
    QStringList failures;
    for (const ADDRINFOW *address = addresses; address; address = address->ai_next) {
        const SOCKET socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        QString error;
        if (socket == INVALID_SOCKET) {
            error = systemError(WSAGetLastError());
        } else {
            u_long nonBlocking = 1;
            if (ioctlsocket(socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
                error = systemError(WSAGetLastError());
            else
                error = connectAddress(socket, *address, timeoutMs);
            if (error.isEmpty()) {
                result.socket = socket;
                break;
            }
            closesocket(socket);
        }
        wchar_t numeric[NI_MAXHOST] = {};
        const QString target = GetNameInfoW(address->ai_addr, socklen_t(address->ai_addrlen), numeric,
                                            NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) == 0
            ? QString::fromWCharArray(numeric) : host;
        failures << QStringLiteral("%1 · %2").arg(target, error);
    }
    if (addresses)
        FreeAddrInfoW(addresses);
    if (result.socket == INVALID_SOCKET)
        result.error = failures.isEmpty() ? QCoreApplication::translate("WindowsTcp", "No usable network address found.")
                                          : failures.join(QStringLiteral("; "));
    return result;
}

} // namespace WindowsTcp
