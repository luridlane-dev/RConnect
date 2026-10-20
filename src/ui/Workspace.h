#pragma once
#include "rdp/RdpSession.h"

#include <QHash>
#include <QList>
#include <QObject>

#include <string>

class HomePage;
class QFrame;
class QPoint;
class QScreen;
class QWidget;
class ServerStore;
class SessionWindow;
class StatusMonitor;

// Owns the server list, the windows and the sessions.
class Workspace : public QObject {
    Q_OBJECT
public:
    // windowMarker: window property used to find this instance.
    explicit Workspace(const wchar_t *windowMarker, QObject *parent = nullptr);
    ~Workspace() override;

    void start();

    SessionWindow *activeSessionWindow() const;

private:
    SessionWindow *createWindow();
    SessionWindow *windowOf(const QString &serverId) const;
    QWidget *dialogParent() const;

    void openServer(const QString &id, SessionWindow *target);
    void closeSession(const QString &id); // asks while connected
    void endSession(const QString &id);
    void detachSession(const QString &id, const QPoint &globalPos);
    void dropSession(const QString &id, SessionWindow *target, int index);
    void placeWindow(SessionWindow *w, const QPoint &globalPos, QScreen *sourceScreen);
    void closeWindowIfEmpty(SessionWindow *w);
    void onWindowCloseRequested(SessionWindow *w);
    void quit(bool ask);

    void showServerPopup(SessionWindow *w);
    void addServer();
    void editServer(const QString &id);
    void removeServer(const QString &id);
    void onSessionState(RdpSession::State state);
    void refreshOpenMarkers();

    std::wstring m_marker;
    ServerStore *m_store;
    StatusMonitor *m_monitor;
    HomePage *m_home = nullptr;
    SessionWindow *m_main = nullptr;
    QList<SessionWindow *> m_windows;
    QHash<SessionWindow *, QFrame *> m_popups;
    QHash<QString, RdpSession *> m_sessions;
    bool m_quitting = false;
};
