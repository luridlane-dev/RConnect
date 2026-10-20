#include "Workspace.h"

#include "HomePage.h"
#include "ServerEditDialog.h"
#include "ServerPanel.h"
#include "SessionWindow.h"
#include "core/StatusMonitor.h"
#include "model/ServerStore.h"

#include <QApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <windows.h>

namespace {

bool confirm(QWidget *parent, const QString &title, const QString &text)
{
    QMessageBox box(QMessageBox::Question, title, text, QMessageBox::NoButton, parent);
    QPushButton *yes = box.addButton(Workspace::tr("Yes"), QMessageBox::YesRole);
    box.addButton(Workspace::tr("No"), QMessageBox::NoRole);
    box.setDefaultButton(yes);
    box.exec();
    return box.clickedButton() == yes;
}

// The RDP control takes the keyboard, so app hotkeys are caught with a thread hook.
Workspace *g_workspace = nullptr;
HHOOK g_hook = nullptr;

LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_workspace && !(lParam & (1 << 31))) {
        if (SessionWindow *w = g_workspace->activeSessionWindow()) {
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            const bool alt = GetKeyState(VK_MENU) < 0;
            const bool shift = GetKeyState(VK_SHIFT) < 0;
            auto post = [w](SessionWindow::Hotkey key, int arg) {
                QMetaObject::invokeMethod(w, [w, key, arg] { w->handleHotkey(key, arg); }, Qt::QueuedConnection);
                return LRESULT(1);
            };
            if (wParam == VK_F11 && !ctrl && !alt)
                return post(SessionWindow::ToggleFullscreen, 0);
            if (wParam == VK_TAB && ctrl && !alt)
                return post(shift ? SessionWindow::PrevTab : SessionWindow::NextTab, 0);
            if (alt && !ctrl && wParam >= '1' && wParam <= '9')
                return post(SessionWindow::GoToTab, int(wParam - '1'));
        }
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

} // namespace

Workspace::Workspace(const wchar_t *windowMarker, QObject *parent)
    : QObject(parent)
    , m_marker(windowMarker)
    , m_store(new ServerStore(this))
    , m_monitor(new StatusMonitor(m_store, this))
{
    m_home = new HomePage(m_store);
    ServerPanel *panel = m_home->panel();
    connect(panel, &ServerPanel::openRequested, this, [this](const QString &id) { openServer(id, m_main); });
    connect(panel, &ServerPanel::addRequested, this, &Workspace::addServer);
    connect(panel, &ServerPanel::editRequested, this, &Workspace::editServer);
    connect(panel, &ServerPanel::removeRequested, this, &Workspace::removeServer);
    connect(panel, &ServerPanel::refreshRequested, this, [this](const QString &id) { m_monitor->refresh(id, true); });

    m_main = createWindow();
    SetPropW(reinterpret_cast<HWND>(m_main->winId()), m_marker.c_str(), HANDLE(1));
    connect(m_main, &SessionWindow::minimizedChanged, this, [this](bool minimized) {
        m_monitor->setActive(!minimized);
    });

    // Windows sign-out or shutdown
    connect(qApp, &QGuiApplication::commitDataRequest, this, [this] { quit(false); });

    g_workspace = this;
    g_hook = SetWindowsHookExW(WH_KEYBOARD, keyboardProc, nullptr, GetCurrentThreadId());
}

Workspace::~Workspace()
{
    if (g_hook)
        UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    g_workspace = nullptr;
    const QList<SessionWindow *> windows = m_windows;
    for (SessionWindow *w : windows)
        if (w != m_main)
            delete w;
    if (m_main)
        RemovePropW(reinterpret_cast<HWND>(m_main->winId()), m_marker.c_str());
    delete m_main;
}

void Workspace::start()
{
    m_store->load();
    m_main->restoreGeometry(QSettings().value(QStringLiteral("geometry")).toByteArray());
    m_main->showFullScreen();
    m_monitor->setActive(true);
    m_monitor->resolveCountries();
}

SessionWindow *Workspace::activeSessionWindow() const
{
    return qobject_cast<SessionWindow *>(QApplication::activeWindow());
}

SessionWindow *Workspace::createWindow()
{
    auto *w = new SessionWindow(m_main ? nullptr : m_home);
    m_windows << w;
    w->setNotesProvider([this](const QString &id) { return m_store->profileById(id).notes; });

    // Queued: a move may delete the window that started the drag.
    connect(w, &SessionWindow::serversRequested, this, &Workspace::showServerPopup);
    connect(w, &SessionWindow::closeTabRequested, this, &Workspace::closeSession);
    connect(w, &SessionWindow::detachRequested, this, &Workspace::detachSession, Qt::QueuedConnection);
    connect(w, &SessionWindow::dropRequested, this, &Workspace::dropSession, Qt::QueuedConnection);
    connect(w, &SessionWindow::moveToMainRequested, this,
            [this](const QString &id) { dropSession(id, m_main, -1); }, Qt::QueuedConnection);
    connect(w, &SessionWindow::addServerRequested, this, &Workspace::addServer);
    connect(w, &SessionWindow::refreshStatusRequested, m_monitor, &StatusMonitor::refreshAll);
    connect(w, &SessionWindow::closeRequested, this, &Workspace::onWindowCloseRequested);
    connect(w, &QObject::destroyed, this, [this, w] {
        m_windows.removeAll(w);
        m_popups.remove(w);
    });
    return w;
}

SessionWindow *Workspace::windowOf(const QString &serverId) const
{
    for (SessionWindow *w : m_windows)
        if (w->session(serverId))
            return w;
    return nullptr;
}

QWidget *Workspace::dialogParent() const
{
    if (SessionWindow *w = activeSessionWindow())
        return w;
    return m_main;
}


void Workspace::openServer(const QString &id, SessionWindow *target)
{
    if (SessionWindow *w = windowOf(id)) {
        w->activateSession(id);
        return;
    }
    ServerProfile p = m_store->profileById(id);
    if (!p.isValid())
        return;

    if (!p.savePassword) {
        bool ok = false;
        const QString pw = QInputDialog::getText(target, p.name,
                                                 tr("Password for %1:").arg(p.displayUser().isEmpty() ? p.host : p.displayUser()),
                                                 QLineEdit::Password, QString(), &ok);
        if (!ok)
            return;
        p.password = pw;
    }

    auto *session = new RdpSession(p, target);
    m_sessions.insert(id, session);
    connect(session, &RdpSession::stateChanged, this, &Workspace::onSessionState);
    connect(session, &RdpSession::closeRequested, this, [this, id] { closeSession(id); });
    target->addSession(session);
    refreshOpenMarkers();
    session->start();
}

void Workspace::closeSession(const QString &id)
{
    RdpSession *s = m_sessions.value(id);
    if (!s)
        return;
    if (s->state() == RdpSession::Connected
        && !confirm(windowOf(id), tr("Close connection"), tr("Close the connection to “%1”?").arg(s->serverName())))
        return;
    endSession(id);
}

void Workspace::endSession(const QString &id)
{
    RdpSession *s = m_sessions.take(id);
    if (!s)
        return;
    SessionWindow *w = windowOf(id);
    if (w)
        w->takeSession(id);
    s->shutdown();
    if (w)
        closeWindowIfEmpty(w);
    refreshOpenMarkers();
    m_monitor->refresh(id);
}

void Workspace::detachSession(const QString &id, const QPoint &globalPos)
{
    SessionWindow *from = windowOf(id);
    if (!from)
        return;
    QScreen *sourceScreen = from->screen();
    if (!from->isMain() && from->sessionCount() == 1) {
        placeWindow(from, globalPos, sourceScreen);
        return;
    }
    SessionWindow *to = createWindow();
    to->addSession(from->takeSession(id));
    placeWindow(to, globalPos, sourceScreen);
    closeWindowIfEmpty(from);
}

void Workspace::dropSession(const QString &id, SessionWindow *target, int index)
{
    SessionWindow *from = windowOf(id);
    if (!from || !m_windows.contains(target))
        return;
    if (from == target) {
        target->moveTab(id, index);
        return;
    }
    target->addSession(from->takeSession(id), index);
    target->activateSession(id);
    closeWindowIfEmpty(from);
}

void Workspace::placeWindow(SessionWindow *w, const QPoint &globalPos, QScreen *sourceScreen)
{
    QScreen *screen = QGuiApplication::screenAt(globalPos);
    if (!screen)
        screen = sourceScreen;
    const QRect avail = screen->availableGeometry();
    if (w->isFullScreen())
        w->showNormal();

    if (screen != sourceScreen) {
        w->setGeometry(avail.adjusted(avail.width() / 8, avail.height() / 8, -avail.width() / 8, -avail.height() / 8));
        w->showFullScreen();
    } else {
        QRect r(QPoint(0, 0), QSize(avail.width() * 3 / 4, avail.height() * 3 / 4));
        r.moveTopLeft(globalPos - QPoint(r.width() / 4, 12));
        r.moveLeft(qBound(avail.left(), r.left(), avail.right() - r.width()));
        r.moveTop(qBound(avail.top(), r.top(), avail.bottom() - r.height()));
        w->setGeometry(r);
        w->show();
    }
    w->raise();
    w->activateWindow();
}

void Workspace::closeWindowIfEmpty(SessionWindow *w)
{
    if (w == m_main || w->sessionCount() > 0)
        return;
    // Deleted later so closing sessions can disconnect cleanly.
    w->hide();
    QTimer::singleShot(2000, w, [w] {
        w->allowClose();
        w->close();
    });
}

void Workspace::onWindowCloseRequested(SessionWindow *w)
{
    if (w == m_main) {
        quit(true);
        return;
    }
    const QList<RdpSession *> sessions = w->sessions();
    bool anyConnected = false;
    for (RdpSession *s : sessions)
        anyConnected |= s->state() == RdpSession::Connected;
    if (anyConnected
        && !confirm(w, tr("Close window"), tr("Close the connections in this window?")))
        return;
    for (RdpSession *s : sessions)
        endSession(s->serverId());
    closeWindowIfEmpty(w);
}

void Workspace::quit(bool ask)
{
    if (m_quitting)
        return;
    bool anyConnected = false;
    for (RdpSession *s : std::as_const(m_sessions))
        anyConnected |= s->state() == RdpSession::Connected;
    if (ask && anyConnected
        && !confirm(dialogParent(), tr("Quit"),
                    tr("There are open connections. Close all of them and quit RConnect?")))
        return;

    m_quitting = true;
    QSettings().setValue(QStringLiteral("geometry"), m_main->saveGeometry());
    m_monitor->setActive(false);
    for (RdpSession *s : std::as_const(m_sessions))
        s->shutdown();
    m_sessions.clear();
    for (SessionWindow *w : std::as_const(m_windows)) {
        w->allowClose();
        w->hide();
    }
    QApplication::quit();
}


void Workspace::showServerPopup(SessionWindow *w)
{
    QFrame *popup = m_popups.value(w);
    if (!popup) {
        // Popup, so it shows above the native RDP window.
        popup = new QFrame(w, Qt::Popup | Qt::FramelessWindowHint);
        popup->setObjectName(QStringLiteral("popup"));
        auto *lay = new QVBoxLayout(popup);
        lay->setContentsMargins(1, 1, 1, 1);
        auto *panel = new ServerPanel(m_store, popup);
        lay->addWidget(panel);
        connect(panel, &ServerPanel::openRequested, this, [this, w, popup](const QString &id) {
            popup->hide();
            openServer(id, w);
        });
        connect(panel, &ServerPanel::addRequested, this, [this, popup] { popup->hide(); addServer(); });
        connect(panel, &ServerPanel::editRequested, this, [this, popup](const QString &id) { popup->hide(); editServer(id); });
        connect(panel, &ServerPanel::removeRequested, this, [this, popup](const QString &id) { popup->hide(); removeServer(id); });
        connect(panel, &ServerPanel::refreshRequested, this, [this](const QString &id) { m_monitor->refresh(id, true); });
        m_popups.insert(w, popup);
        refreshOpenMarkers();
    }
    auto *panel = popup->findChild<ServerPanel *>();
    QToolButton *anchor = w->serversButton();
    popup->resize(qMin(790, w->width() - 40), qMin(560, w->height() - 60));
    popup->move(anchor->mapToGlobal(QPoint(0, anchor->height() + 2)));
    panel->clearSearch();
    popup->show();
    panel->focusSearch();
}

void Workspace::addServer()
{
    ServerEditDialog dlg(ServerProfile{}, true, m_store->usedColors(), dialogParent());
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_store->add(dlg.profile());
    m_monitor->refreshAll();
    m_monitor->resolveCountries();
}

void Workspace::editServer(const QString &id)
{
    const ServerProfile cur = m_store->profileById(id);
    if (!cur.isValid())
        return;
    ServerEditDialog dlg(cur, false, m_store->usedColors(), dialogParent());
    if (dlg.exec() != QDialog::Accepted)
        return;
    const ServerProfile updated = dlg.profile();
    m_store->update(updated);
    m_monitor->refresh(id);
    m_monitor->resolveCountries();
    if (RdpSession *s = m_sessions.value(id)) {
        s->setServerName(updated.name);
        if (SessionWindow *w = windowOf(id)) {
            w->updateSessionTab(s);
            w->updateNotesButton();
        }
    }
}

void Workspace::removeServer(const QString &id)
{
    const ServerProfile p = m_store->profileById(id);
    if (!p.isValid())
        return;
    if (!confirm(dialogParent(), tr("Delete server"), tr("Remove “%1” from the list?").arg(p.name)))
        return;
    endSession(id);
    m_store->remove(id);
}

void Workspace::onSessionState(RdpSession::State state)
{
    auto *s = qobject_cast<RdpSession *>(sender());
    if (!s)
        return;
    SessionWindow *w = windowOf(s->serverId());
    if (w) {
        w->updateSessionTab(s, state == RdpSession::Replaced);
        if (state == RdpSession::Replaced)
            QApplication::alert(w, 0);
    }
    if (state == RdpSession::Connected || state == RdpSession::Replaced)
        m_monitor->refresh(s->serverId());
}

void Workspace::refreshOpenMarkers()
{
    QSet<QString> ids;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it)
        ids.insert(it.key());
    m_home->panel()->setOpenIds(ids);
    for (QFrame *popup : std::as_const(m_popups))
        popup->findChild<ServerPanel *>()->setOpenIds(ids);
}
