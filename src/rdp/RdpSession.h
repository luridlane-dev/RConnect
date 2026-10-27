#pragma once
#include "model/ServerProfile.h"

#include <QTimer>
#include <QWidget>

class QAxWidget;
class QStackedLayout;
class SessionOverlay;

// One RDP connection: the ActiveX control and an overlay for the connect/disconnect states.
class RdpSession : public QWidget {
    Q_OBJECT
public:
    enum State { Idle, Connecting, Connected, Disconnected, Replaced, Failed };
    Q_ENUM(State)

    explicit RdpSession(const ServerProfile &profile, QWidget *parent = nullptr);

    QString serverId() const { return m_profile.id; }
    QString serverName() const { return m_profile.name; }
    void setServerName(const QString &name) { m_profile.name = name; }
    State state() const { return m_state; }

    void start();
    void reconnect();
    // Reconnects without closing the tab.
    void refresh();
    // Disconnects, then deletes itself.
    void shutdown();

signals:
    void stateChanged(RdpSession::State state);
    void closeRequested();

protected:
    void resizeEvent(QResizeEvent *e) override;
    void changeEvent(QEvent *e) override;

private slots:
    void onConnecting();
    void onConnected();
    void onLoginComplete();
    void onDisconnected(int reason);

private:
    bool createControl();
    void applySettings();
    // The control's COM events don't reach Qt, so Connected is polled (0 off, 1 on, 2 connecting).
    void poll();
    void handleDisconnected(int reason);
    QSize remoteSize() const;
    uint desktopScale() const;
    uint deviceScale() const;
    void syncDisplaySize();
    void setState(State s);
    void showOverlay();
    QString describe(int reason, int extReason) const;

    ServerProfile m_profile;
    QAxWidget *m_ax = nullptr;
    QStackedLayout *m_stack = nullptr;
    SessionOverlay *m_overlay = nullptr;
    State m_state = Idle;
    bool m_everConnected = false;
    bool m_closing = false;
    bool m_sawActive = false;
    bool m_refreshing = false;
    bool m_controlMissing = false;
    int m_lastReason = 0;
    int m_lastExtReason = 0;
    QSize m_syncedSize; // last size sent to the server
    QTimer m_poll;
    QTimer m_resizeTimer;
};
