#pragma once
#include "AccessLog.h"
#include "WinRm.h"
#include "model/ServerProfile.h"
#include "model/ServerStatus.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

#include <functional>
#include <memory>

class ServerStore;

// Optional TCP check of each server's RDP port, and of who is connected (WinRM).
class StatusMonitor : public QObject {
    Q_OBJECT
public:
    explicit StatusMonitor(ServerStore *store, QObject *parent = nullptr);
    ~StatusMonitor() override;

    void setActive(bool active);

    static bool canCheckSessions(const ServerProfile &p);
    // Asks the server who is connected now, updates the store, then calls `done`.
    void checkSessions(const ServerProfile &p, std::function<void(const ServerStatus &)> done);

public slots:
    void refreshAll();
    // Asks every server with a session check now, however recent the last answer.
    void refreshAllNow();
    void refresh(const QString &id, bool force = false);
    // Looks up the country of servers whose host changed (offline table, no traffic to the server).
    void resolveCountries();

signals:
    // Something in the servers' logs deserves attention (see Threats). The record's event is the kind.
    void alertRaised(const AccessRecord &alert);
    // Someone whose computer is in nobody's address book connected to a server.
    void unknownPerson(const QString &server, const QString &client, const QString &user);

private:
    struct Channel; // outlives this object for running probes

    void finished(const QString &id, int latency, bool checked, bool withEvents, const SessionQuery &q);
    static void applySessions(ServerStatus &s, const SessionQuery &q);
    void warnAboutStrangers(const QString &id, const ServerStatus &s);

    ServerStore *m_store;
    QTimer m_timer;
    QSet<QString> m_inFlight;
    QSet<QString> m_retried;
    QSet<QString> m_strangers; // "server id|computer" already warned about while still connected
    QHash<QString, qint64> m_eventsAt; // server id -> when its logs were last read (ms since epoch)
    QThreadPool m_pool; // background checks; interactive ones use the global pool
    std::shared_ptr<Channel> m_channel;
};
