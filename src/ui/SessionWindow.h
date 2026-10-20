#pragma once
#include "rdp/RdpSession.h"

#include <QHash>
#include <QList>
#include <QWidget>

#include <functional>

class AnimatedTabBar;
class QStackedWidget;
class QToolButton;

// Window with tabs and sessions. The main window also has the start page.
class SessionWindow : public QWidget {
    Q_OBJECT
public:
    explicit SessionWindow(QWidget *home, QWidget *parent = nullptr);

    bool isMain() const { return m_home != nullptr; }

    void addSession(RdpSession *s, int index = -1);
    RdpSession *takeSession(const QString &serverId);
    RdpSession *session(const QString &serverId) const { return m_sessions.value(serverId); }
    RdpSession *currentSession() const;
    QList<RdpSession *> sessions() const { return m_sessions.values(); }
    int sessionCount() const { return int(m_sessions.size()); }
    void activateSession(const QString &serverId);
    void moveTab(const QString &serverId, int index);
    void updateSessionTab(RdpSession *s, bool attention = false);

    void setNotesProvider(std::function<QString(const QString &)> provider);
    void updateNotesButton();

    enum Hotkey { NextTab, PrevTab, ToggleFullscreen, GoToTab };
    void handleHotkey(Hotkey key, int arg = 0);
    void toggleFullscreen();
    void allowClose() { m_allowClose = true; }

    QToolButton *serversButton() const;

signals:
    void serversRequested(SessionWindow *window);
    void closeTabRequested(const QString &serverId);
    void detachRequested(const QString &serverId, const QPoint &globalPos);
    void dropRequested(const QString &serverId, SessionWindow *target, int index);
    void moveToMainRequested(const QString &serverId);
    void addServerRequested();
    void refreshStatusRequested();
    void closeRequested(SessionWindow *window);
    void minimizedChanged(bool minimized);

protected:
    void changeEvent(QEvent *e) override;
    void closeEvent(QCloseEvent *e) override;

private:
    int tabIndexOf(const QString &serverId) const;
    void onTabChanged(int index);
    void showTabMenu(const QPoint &pos);
    void showNotes();
    void updatePage();
    void updateTitle();
    void retranslate();

    QWidget *m_home = nullptr;
    QWidget *m_topBar = nullptr;
    AnimatedTabBar *m_tabBar = nullptr;
    QToolButton *m_notesButton = nullptr;
    QList<QToolButton *> m_winButtons;
    QStackedWidget *m_pages = nullptr;
    QStackedWidget *m_sessionStack = nullptr;
    QHash<QString, RdpSession *> m_sessions;
    std::function<QString(const QString &)> m_notes;
    bool m_allowClose = false;
};
