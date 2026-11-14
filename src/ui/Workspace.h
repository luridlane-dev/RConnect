#pragma once
#include "core/AccessLog.h"
#include "core/UpdateChecker.h"
#include "rdp/RdpSession.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>

#include <optional>
#include <string>

class HomePage;
class QFrame;
class QPoint;
class QScreen;
class QSystemTrayIcon;
class QWidget;
class ServerStore;
class SessionWindow;
class StatusMonitor;
struct ServerProfile;
struct ServerStatus;

// Owns the server list, the windows and the sessions.
class Workspace : public QObject {
    Q_OBJECT
public:
    // windowMarker: window property used to find this instance.
    explicit Workspace(const wchar_t *windowMarker, QObject *parent = nullptr);
    ~Workspace() override;

    // startMinimized: opened by Windows at sign-in; stays in the taskbar and keeps watching the servers.
    void start(bool startMinimized = false);

    SessionWindow *activeSessionWindow() const;

private:
    SessionWindow *createWindow();
    SessionWindow *windowOf(const QString &serverId) const;
    QWidget *dialogParent() const;

    void openServer(const QString &id, SessionWindow *target); // warns if someone else is connected
    void connectServer(const ServerProfile &p, SessionWindow *target);
    bool confirmBusy(const ServerProfile &p, const ServerStatus &status, QWidget *parent) const;
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
    void importServers();
    void exportServers();
    void editServer(const QString &id);
    void removeServer(const QString &id);
    void onSessionState(RdpSession::State state);
    void updateLiveSessions();
    void explainEnd(RdpSession *s);
    void refreshOpenMarkers();
    void enableAuditing(const QString &id);
    // Lets the server send up to 60 frames a second and full-colour (sharper) text.
    void tuneDisplay(const QString &id);
    void showAlert(const AccessRecord &alert);
    void showStranger(const QString &server, const QString &client, const QString &user);
    QSystemTrayIcon *tray();
    void sendMessage(const QString &serverId, int sessionId, const QString &user);
    // Detailed logging only: what was done in RConnect, written to the activity history.
    static void audit(const ServerProfile &p, const QString &event, const QString &detail = {});

    // manual: started from the Settings menu, so it also reports "up to date" and errors.
    void checkForUpdates(bool manual);
    void onUpdateChecked(bool ok, bool available, const UpdateInfo &info, const QString &error);
    void offerUpdate(const UpdateInfo &info);
    void installUpdate(const UpdateInfo &info);

    std::wstring m_marker;
    ServerStore *m_store;
    StatusMonitor *m_monitor;
    UpdateChecker *m_updater;
    bool m_updateChecking = false;
    bool m_manualCheck = false;
    bool m_updateDialogOpen = false;
    std::optional<UpdateInfo> m_pendingUpdate; // found while the window was minimized
    HomePage *m_home = nullptr;
    SessionWindow *m_main = nullptr;
    QList<SessionWindow *> m_windows;
    QHash<SessionWindow *, QFrame *> m_popups;
    QSystemTrayIcon *m_tray = nullptr; // created with the first alert
    QHash<QString, RdpSession *> m_sessions;
    QSet<QString> m_pendingOpens; // waiting for the session check
    bool m_quitting = false;
};
