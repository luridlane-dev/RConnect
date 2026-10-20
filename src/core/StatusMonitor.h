#pragma once
#include <QObject>
#include <QSet>
#include <QTimer>

#include <memory>

class ServerStore;

// Optional TCP check of each server's RDP port.
class StatusMonitor : public QObject {
    Q_OBJECT
public:
    explicit StatusMonitor(ServerStore *store, QObject *parent = nullptr);
    ~StatusMonitor() override;

    void setActive(bool active);

public slots:
    void refreshAll();
    void refresh(const QString &id, bool force = false);
    // Looks up the country of servers whose host changed (offline table, no traffic to the server).
    void resolveCountries();

private:
    struct Channel; // outlives this object for running probes

    void finished(const QString &id, bool online);

    ServerStore *m_store;
    QTimer m_timer;
    QSet<QString> m_inFlight;
    std::shared_ptr<Channel> m_channel;
};
