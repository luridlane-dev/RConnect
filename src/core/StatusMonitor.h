#pragma once
#include "WinRm.h"
#include "model/ServerProfile.h"
#include "model/ServerStatus.h"

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
    void refresh(const QString &id, bool force = false);
    // Looks up the country of servers whose host changed (offline table, no traffic to the server).
    void resolveCountries();

private:
    struct Channel; // outlives this object for running probes

    void finished(const QString &id, int latency, bool checked, const SessionQuery &q);
    static void applySessions(ServerStatus &s, const SessionQuery &q);

    ServerStore *m_store;
    QTimer m_timer;
    QSet<QString> m_inFlight;
    QSet<QString> m_retried;
    QThreadPool m_pool; // background checks; interactive ones use the global pool
    std::shared_ptr<Channel> m_channel;
};
