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
#include <optional>

class ServerStore;

// Optional TCP check of each server's RDP port, and of who is connected (WinRM).
class StatusMonitor : public QObject {
    Q_OBJECT
public:
    explicit StatusMonitor(ServerStore *store, QObject *parent = nullptr);
    ~StatusMonitor() override;

    void setActive(bool active);

    static bool canCheckSessions(const ServerProfile &p);
    std::optional<ServerStatus> recentSessionStatus(const ServerProfile &p) const;
    // Asks the server who is connected now, updates the store, then calls `done`.
    void checkSessions(const ServerProfile &p, std::function<void(const ServerStatus &)> done);
    void localConnectionChanged(const QString &id);

public slots:
    void refreshAll();
    // Asks every server with a session check now, however recent the last answer.
    void refreshAllNow();
    void refreshEvents(const QString &serverId);
    void refresh(const QString &id, bool force = false);
    // Looks up the country of servers whose host changed (offline table, no traffic to the server).
    void resolveCountries();

signals:
    // Something in the servers' logs deserves attention (see Threats). The record's event is the kind.
    void alertRaised(const AccessRecord &alert);
    // Someone whose computer is in nobody's address book connected to a server.
    void unknownPerson(const QString &server, const QString &client, const QString &user);
    void eventSyncChanged(const QString &id, const QString &server, bool running, const QStringList &failedChannels,
                          const QString &error);

private:
    struct Channel; // outlives this object for running probes
    struct SessionCheck;

    void finishProbe(const ServerProfile &p, int latency);
    void requestSessions(const ServerProfile &p, bool includeTimes, std::function<void(const ServerStatus &)> done = {});
    void startSessionCheck(const std::shared_ptr<SessionCheck> &check, QThreadPool &pool);
    void finishSessions(const std::shared_ptr<SessionCheck> &check, const SessionQuery &q);
    void pollEvents(const ServerProfile &p, bool force = false);
    void finishEventPoll(const ServerProfile &p, const SessionQuery &q);
    static void applySessions(ServerStatus &s, const SessionQuery &q);
    void warnAboutStrangers(const QString &id, const ServerStatus &s);

    ServerStore *m_store;
    QTimer m_timer;
    QSet<QString> m_probes;
    QSet<QString> m_forcedProbes;
    QSet<QString> m_retried;
    QSet<QString> m_strangers; // "server id|computer" already warned about while still connected
    QSet<QString> m_eventPolls;
    QHash<QString, qint64> m_eventsAt;
    QHash<QString, qint64> m_usageAt;
    QHash<QString, qint64> m_sessionTimesAt;
    QHash<QString, std::shared_ptr<SessionCheck>> m_sessionChecks;
    QThreadPool m_probePool;
    QThreadPool m_sessionPool;
    QThreadPool m_interactivePool;
    QThreadPool m_eventPool;
    std::shared_ptr<Channel> m_channel;
};
