#pragma once
#include "model/ServerProfile.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>
#include <QWidget>

class QAxWidget;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
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
    // Gives the keyboard to the control's inner window; only then Win and Alt+Tab go to the server.
    void focusRemote();
    // After the connection ended: what the server said about who is connected. An empty title keeps the default.
    void setServerView(const QString &title, const QString &message);

signals:
    void stateChanged(RdpSession::State state);
    void closeRequested();

protected:
    void resizeEvent(QResizeEvent *e) override;
    void changeEvent(QEvent *e) override;
    void showEvent(QShowEvent *e) override;
    // Files dropped on the session are copied to the server through the shared clipboard.
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private slots:
    void onConnecting();
    void onConnected();
    void onLoginComplete();
    void onDisconnected(int reason);

private:
    bool createControl();
    // A control that has been connected once is never reused: a dropped one can stall on the next Connect().
    void replaceControl();
    bool worthRetrying(bool wasConnected) const;
    // Schedules the next automatic attempt with a fresh control; false when the retries are used up.
    bool scheduleRetry();
    // The attempt went nowhere (Connect() never started, or it hung): retry or show why.
    void attemptFailed(int reason, bool retry);
    // Makes the control repaint everything, so no blank surface is left after the overlay goes away.
    void repaintRemote();
    void applySettings();
    // The control's COM events don't reach Qt, so Connected is polled (0 off, 1 on, 2 connecting).
    void poll();
    void handleDisconnected(int reason);
    QSize remoteSize() const;
    uint desktopScale() const;
    uint deviceScale() const;
    void syncDisplaySize();
    void setState(State s);
    void log(const QString &text) const;
    void showOverlay();
    void sendFiles(const QList<QUrl> &files);
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
    QString m_noteTitle, m_noteMessage; // see setServerView()
    int m_retryAttempt = 0; // automatic reconnects since the last good connection
    QTimer m_retryTimer;
    QSize m_syncedSize; // last size sent to the server
    QElapsedTimer m_connectClock;
    qint64 m_lastHeartbeat = 0;
    qint64 m_connectCalledAt = -1; // m_connectClock time when Connect() returned; -1 before
    qint64 m_refreshSince = 0;     // m_connectClock time when refresh() asked for the disconnect
    int m_lastPolled = -1;
    QTimer m_poll;
    QTimer m_resizeTimer;
};
