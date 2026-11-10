#pragma once
#include <QObject>
#include <QString>

// Computer names for IP addresses (reverse lookup), for lines of the history that only carry an address: the
// computer that tried to sign in is often only known by its IP. Looked up in the background and remembered.
namespace HostNames {

// The name if it is known already; empty if there is none or it has not been looked up yet.
QString cached(const QString &address);

// Looks the address up in the background (once); `resolved` is emitted when a name came in.
void request(const QString &address);

// Addresses inside this network (10.x, 172.16-31.x, 192.168.x, fe80::, ...): a name there is a real computer name.
bool isLocalAddress(const QString &address);

class Notifier : public QObject {
    Q_OBJECT
signals:
    void resolved();
};
Notifier *notifier();

} // namespace HostNames
