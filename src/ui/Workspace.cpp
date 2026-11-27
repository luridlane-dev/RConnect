#include "Workspace.h"

#include "AccessLogPage.h"
#include "HomePage.h"
#include "ServerEditDialog.h"
#include "ServerPanel.h"
#include "SessionWindow.h"
#include "Theme.h"
#include "UsagePage.h"
#include "core/AccessLog.h"
#include "core/Contacts.h"
#include "core/JumpList.h"
#include "core/Log.h"
#include "core/Priority.h"
#include "core/ServerImport.h"
#include "core/StatusMonitor.h"
#include "core/Threats.h"
#include "core/WinRm.h"
#include "model/ServerStore.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QListWidget>
#include <QMutex>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QSet>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <optional>

#include <windows.h>

namespace {

bool confirm(QWidget *parent, const QString &title, const QString &text, bool defaultYes = true)
{
    QMessageBox box(defaultYes ? QMessageBox::Question : QMessageBox::Warning, title, text, QMessageBox::NoButton,
                    parent);
    QPushButton *yes = box.addButton(Workspace::tr("Yes"), QMessageBox::YesRole);
    QPushButton *no = box.addButton(Workspace::tr("No"), QMessageBox::NoRole);
    box.setDefaultButton(defaultYes ? yes : no);
    box.exec();
    return box.clickedButton() == yes;
}

// Opaque background for the server popup. Painted by hand because the window is
// translucent (rounded corners) and a style sheet does not fill it reliably.
class PopupFrame : public QFrame {
public:
    using QFrame::QFrame;

    std::function<void()> onHidden;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setPen(QPen(Theme::kPopupBorder, 1));
        p.setBrush(Theme::kPopupFill);
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }

    void hideEvent(QHideEvent *e) override
    {
        QFrame::hideEvent(e);
        if (onHidden)
            onHidden();
    }
};

// The RDP control takes the keyboard, so app hotkeys are caught with a thread hook.
Workspace *g_workspace = nullptr;
HHOOK g_hook = nullptr;

LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_workspace && !(lParam & (1 << 31))
        && (wParam == VK_F11 || wParam == VK_TAB || (wParam >= '1' && wParam <= '9'))
        && !QApplication::activePopupWidget()) {
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
    , m_updater(new UpdateChecker(this))
{
    m_connectLogPool.setMaxThreadCount(1);
    m_home = new HomePage(m_store);
    connect(&Contacts::instance(), &Contacts::changed, m_store, &ServerStore::refreshDisplay);
    connect(m_home, &HomePage::importRequested, this, &Workspace::importServers);
    connect(m_home, &HomePage::exportRequested, this, &Workspace::exportServers);
    connect(m_home, &HomePage::configureServersRequested, this, &Workspace::configureServerLogging);
    connect(m_home, &HomePage::serverLayoutChanged, this, [this] {
        for (QFrame *popup : std::as_const(m_popups))
            if (auto *panel = popup->findChild<ServerPanel *>())
                panel->reloadLayoutSettings();
    });
    connect(m_home, &HomePage::updateCheckRequested, this, [this] { checkForUpdates(true); });
    connect(m_updater, &UpdateChecker::checked, this, &Workspace::onUpdateChecked);
    connect(m_monitor, &StatusMonitor::alertRaised, this, &Workspace::showAlert);
    connect(m_home->accessLog(), &AccessLogPage::syncRequested, m_monitor, &StatusMonitor::refreshEvents);
    connect(m_monitor, &StatusMonitor::eventSyncChanged, m_home->accessLog(), &AccessLogPage::setSyncState);
    connect(m_home->usage(), &UsagePage::syncRequested, m_monitor, &StatusMonitor::refreshEvents);
    connect(m_monitor, &StatusMonitor::eventSyncChanged, m_home->usage(), &UsagePage::onServerSynced);
    connect(m_monitor, &StatusMonitor::unknownPerson, this, &Workspace::showStranger);
    ServerPanel *panel = m_home->panel();
    connect(panel, &ServerPanel::messageRequested, this, &Workspace::sendMessage);
    connect(panel, &ServerPanel::openRequested, this, [this](const QString &id) { openServer(id, m_main); });
    connect(panel, &ServerPanel::addRequested, this, &Workspace::addServer);
    connect(panel, &ServerPanel::importRequested, this, &Workspace::importServers);
    connect(panel, &ServerPanel::editRequested, this, &Workspace::editServer);
    connect(panel, &ServerPanel::removeRequested, this, &Workspace::removeServer);
    connect(panel, &ServerPanel::refreshRequested, this, [this](const QString &id) { m_monitor->refresh(id, true); });

    m_main = createWindow();
    SetPropW(reinterpret_cast<HWND>(m_main->winId()), m_marker.c_str(), HANDLE(1));
    connect(m_main, &SessionWindow::connectServerRequested, this, [this](const QString &id) { openServer(id, m_main); });
    connect(m_main, &SessionWindow::languageChanged, this, &Workspace::updateJumpList);
    // Checking goes on while the window is minimized (or in the tray): the history and the time on servers are only
    // as complete as the checks, and a window that stays minimized for hours must not leave a gap in them.
    // An update found while minimized is offered once the window is back.
    connect(m_main, &SessionWindow::minimizedChanged, this, [this](bool minimized) {
        if (!minimized && m_pendingUpdate) {
            const UpdateInfo info = *m_pendingUpdate;
            m_pendingUpdate.reset();
            QTimer::singleShot(0, this, [this, info] { offerUpdate(info); });
        }
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

void Workspace::start(bool startMinimized, const QString &initialServerId)
{
    m_store->load();
    m_main->restoreGeometry(QSettings().value(QStringLiteral("geometry")).toByteArray());
    if (startMinimized)
        m_main->showMinimized();
    else
        m_main->showFullScreen();
    QTimer::singleShot(0, this, &Workspace::updateJumpList);
    if (!initialServerId.isEmpty())
        QTimer::singleShot(120, this, [this, initialServerId] { openServer(initialServerId, m_main); });
    // The "new country" check compares with the history; read it before the first events arrive.
    QThreadPool::globalInstance()->start([] {
        AccessLog::maintain(); // folds password-guessing runs and drops very old lines before anything reads the file
        Threats::warmUp();
    });
    auto *maintenance = new QTimer(this);
    connect(maintenance, &QTimer::timeout, this, [] { QThreadPool::globalInstance()->start([] { AccessLog::maintain(); }); });
    maintenance->start(6 * 3600 * 1000);
    m_monitor->setActive(true);
    m_monitor->resolveCountries();

    // Look for a new release shortly after start, then every hour (at most once per 12 hours).
    auto autoCheck = [this] {
        if (UpdateChecker::autoCheckEnabled() && UpdateChecker::autoCheckDue())
            checkForUpdates(false);
    };
    QTimer::singleShot(20000, this, autoCheck);
    auto *updateTimer = new QTimer(this);
    connect(updateTimer, &QTimer::timeout, this, autoCheck);
    updateTimer->start(3600 * 1000);
}

SessionWindow *Workspace::activeSessionWindow() const
{
    return qobject_cast<SessionWindow *>(QApplication::activeWindow());
}

SessionWindow *Workspace::createWindow()
{
    auto *w = new SessionWindow(m_main ? nullptr : m_home);
    m_windows << w;
    w->setNotesProvider([this](const QString &id) { return m_store->profileById(id).notes; },
                        [this](const QString &id) { return m_store->hasNotes(id); });

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
        // Picking the server again for a tab whose connection ended is a request to connect again.
        if (RdpSession *s = m_sessions.value(id)) {
            const RdpSession::State st = s->state();
            if (st == RdpSession::Disconnected || st == RdpSession::Failed)
                s->reconnect();
        }
        return;
    }
    if (m_pendingOpens.contains(id))
        return;
    const ServerProfile saved = m_store->profileById(id);
    ServerProfile p = saved;
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

    if (!StatusMonitor::canCheckSessions(p)) {
        connectServer(p, target);
        return;
    }
    const QPointer<SessionWindow> window = target;
    const auto connectChecked = [this, p, saved, window](const ServerStatus &status) {
        const auto stillValid = [&] {
            const ServerProfile current = m_store->profileById(p.id);
            return !m_quitting && !windowOf(p.id) && current.isValid() && current.sameConnection(saved)
                && current.savePassword == saved.savePassword && current.checkSessions == saved.checkSessions;
        };
        if (!stillValid())
            return;
        SessionWindow *destination = window ? window.data() : m_main;
        if (confirmBusy(p, status, destination) && stillValid())
            connectServer(p, window ? window.data() : m_main);
    };
    if (const auto status = m_monitor->recentSessionStatus(p)) {
        m_pendingOpens.insert(id, nullptr); // guard against another open while the busy prompt is visible
        connectChecked(*status);
        m_pendingOpens.remove(id);
        return;
    }

    auto *dialog = new QMessageBox(QMessageBox::Information, p.name,
                                   tr("Checking who is connected…"), QMessageBox::Cancel, target);
    dialog->setInformativeText(tr("You can connect now if you do not need to wait for the session check."));
    auto *proceed = dialog->addButton(tr("Connect now"), QMessageBox::AcceptRole);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setDefaultButton(QMessageBox::Cancel);
    m_pendingOpens.insert(id, dialog);
    connect(dialog, &QObject::destroyed, this, [this, id, dialog] {
        if (m_pendingOpens.value(id) == dialog)
            m_pendingOpens.remove(id);
    });
    auto *reveal = new QTimer(dialog);
    reveal->setSingleShot(true);
    connect(reveal, &QTimer::timeout, dialog, [dialog] { dialog->show(); });
    reveal->start(500);

    const QPointer<QMessageBox> pending = dialog;
    const auto finish = [this, id, pending, reveal, connectChecked](bool accepted, const ServerStatus &status) {
        if (!pending || m_pendingOpens.value(id) != pending.data())
            return;
        m_pendingOpens[id] = nullptr;
        reveal->stop();
        pending->hide();
        pending->deleteLater();
        if (accepted)
            connectChecked(status);
        m_pendingOpens.remove(id);
    };
    connect(dialog, &QMessageBox::finished, this, [this, id, dialog, proceed, finish] {
        finish(dialog->clickedButton() == proceed, m_store->status(id));
    });
    m_monitor->checkSessions(p, [finish](const ServerStatus &status) { finish(true, status); });
}

bool Workspace::confirmBusy(const ServerProfile &p, const ServerStatus &status, QWidget *parent) const
{
    if (status.occupancy != ServerStatus::Busy)
        return true;
    QStringList who;
    bool sameUser = false;
    const QString user = p.user.section(QLatin1Char('\\'), -1);
    for (const RemoteSession &r : status.others) {
        who << tr("%1 (user %2, %3)").arg(sessionOwner(r), r.user, idleText(r.idleMinutes));
        sameUser |= r.user.compare(user, Qt::CaseInsensitive) == 0;
    }
    const QString text = tr("Someone is connected to “%1” right now:").arg(p.name) + QStringLiteral("\n\n")
                         + who.join(QLatin1Char('\n')) + QStringLiteral("\n\n")
                         + (sameUser ? tr("They use the same Windows account, so connecting will disconnect them.")
                                     : tr("Connecting may disconnect them."))
                         + QStringLiteral("\n\n") + tr("Connect anyway?");
    return confirm(parent, tr("Server in use"), text, false);
}

void Workspace::connectServer(const ServerProfile &p, SessionWindow *target)
{
    const QString id = p.id;
    recordRecentServer(id);
    ServerProfile logProfile;
    logProfile.id = p.id;
    logProfile.name = p.name;
    logProfile.host = p.host;
    m_connectLogPool.start([logProfile, user = p.displayUser(), when = QDateTime::currentDateTimeUtc()] {
        AccessLog::note(logProfile, QStringLiteral("Connect"), user, {}, {}, when);
    });
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
    const bool hadLocalConnection = m_store->setLocalConnection(id, false);
    updateLiveSessions();
    audit(m_store->profileById(id), QStringLiteral("Closed"));
    SessionWindow *w = windowOf(id);
    if (w)
        w->takeSession(id);
    s->shutdown();
    if (w)
        closeWindowIfEmpty(w);
    refreshOpenMarkers();
    if (hadLocalConnection)
        m_monitor->localConnectionChanged(id);
    else
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
        auto *frame = new PopupFrame(w, Qt::Popup | Qt::FramelessWindowHint);
        frame->onHidden = [host = QPointer<SessionWindow>(w)] {
            QTimer::singleShot(0, qApp, [host] {
                if (!host || !host->isActiveWindow() || QApplication::activePopupWidget() || QApplication::activeModalWidget())
                    return;
                if (RdpSession *s = host->currentSession(); s && s->isVisible())
                    s->focusRemote();
            });
        };
        popup = frame;
        popup->setObjectName(QStringLiteral("popupFrame"));
        popup->setAttribute(Qt::WA_TranslucentBackground); // rounded corners
        auto *lay = new QVBoxLayout(popup);
        lay->setContentsMargins(16, 14, 16, 14);
        lay->setSpacing(12);

        auto *title = new QLabel(tr("Servers"), popup);
        title->setObjectName(QStringLiteral("popupTitle"));
        auto *summary = new QLabel(ServerPanel::summary(m_store), popup);
        summary->setObjectName(QStringLiteral("pageSubtitle"));
        connect(m_store, &ServerStore::summaryChanged, summary, [this, summary] { summary->setText(ServerPanel::summary(m_store)); });
        auto *refresh = new QPushButton(tr("Refresh"), popup);
        refresh->setCursor(Qt::PointingHandCursor);
        connect(refresh, &QPushButton::clicked, this, [this] { m_monitor->refreshAllNow(); });
        auto *header = new QHBoxLayout;
        header->setSpacing(10);
        header->addWidget(title);
        header->addWidget(summary, 1);
        header->addWidget(refresh);
        lay->addLayout(header);

        auto *panel = new ServerPanel(m_store, popup);
        panel->setImportVisible(false);
        lay->addWidget(panel);
        connect(panel, &ServerPanel::openRequested, this, [this, w, popup](const QString &id) {
            popup->hide();
            openServer(id, w);
        });
        connect(panel, &ServerPanel::addRequested, this, [this, popup] { popup->hide(); addServer(); });
        connect(panel, &ServerPanel::importRequested, this, [this, popup] { popup->hide(); importServers(); });
        connect(panel, &ServerPanel::editRequested, this, [this, popup](const QString &id) { popup->hide(); editServer(id); });
        connect(panel, &ServerPanel::removeRequested, this, [this, popup](const QString &id) { popup->hide(); removeServer(id); });
        connect(panel, &ServerPanel::refreshRequested, this, [this](const QString &id) { m_monitor->refresh(id, true); });
        connect(panel, &ServerPanel::messageRequested, this, [this, popup](const QString &id, int sessionId,
                                                                           const QString &user) {
            popup->hide();
            sendMessage(id, sessionId, user);
        });
        m_popups.insert(w, popup);
        refreshOpenMarkers();
    }
    auto *panel = popup->findChild<ServerPanel *>();
    QToolButton *anchor = w->serversButton();
    popup->resize(qMin(790, w->width() - 40), qMin(560, w->height() - 60));
    popup->move(anchor->mapToGlobal(QPoint(0, anchor->height() + 2)));
    panel->clearSearch();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastPopupRefreshMs >= 10000) {
        m_lastPopupRefreshMs = nowMs;
        m_monitor->refreshAllNow(); // what the cards show may be a minute old
    }
    // While an RDP tab is active, mstscax's native child window (IHWindowClass) holds Win32 focus and keeps its
    // keyboard hook installed; a Qt::Popup does not activate itself on Windows, so keystrokes would keep going to
    // the remote server unless Win32 focus is moved back to the owning Qt window first.
    w->setFocus(Qt::OtherFocusReason);
    if (HWND host = reinterpret_cast<HWND>(w->winId()))
        SetFocus(host);
    popup->show();
    popup->raise();
    panel->focusSearch();
    QTimer::singleShot(0, popup, [popup, panel, host = QPointer<SessionWindow>(w)] {
        if (!popup->isVisible())
            return;
        if (host) {
            if (HWND hwnd = reinterpret_cast<HWND>(host->winId()); hwnd && GetFocus() != hwnd)
                SetFocus(hwnd);
        }
        panel->focusSearch();
    });
}

void Workspace::addServer()
{
    ServerEditDialog dlg(ServerProfile{}, true, m_store->groups(), dialogParent());
    if (dlg.exec() != QDialog::Accepted)
        return;
    const ServerProfile added = dlg.profile();
    m_store->add(added);
    audit(added, QStringLiteral("ServerAdded"), QStringLiteral("%1:%2").arg(added.host).arg(added.port));
    m_monitor->refreshAll();
    m_monitor->resolveCountries();
}

void Workspace::audit(const ServerProfile &p, const QString &event, const QString &detail)
{
    // Adding a server and every connection RConnect opens or closes are always recorded; the rest only with detailed logging.
    if (Log::detailed() || event == QLatin1String("ServerAdded") || event == QLatin1String("Closed")) {
        ServerProfile logProfile;
        logProfile.id = p.id;
        logProfile.name = p.name;
        logProfile.host = p.host;
        const QString user = p.displayUser();
        const QDateTime when = QDateTime::currentDateTimeUtc();
        m_connectLogPool.start([logProfile, event, user, detail, when] {
            AccessLog::note(logProfile, event, user, detail, {}, when);
        });
    }
}

void Workspace::importServers()
{
    const QString path = QFileDialog::getOpenFileName(dialogParent(), tr("Import servers"), QString(),
                                                      tr("Server list (*.json *.db)"));
    if (path.isEmpty())
        return;

    ServerImport::Result res = ServerImport::load(path);
    QString passphrase;
    while (res.status == ServerImport::Status::NeedPassphrase || res.status == ServerImport::Status::WrongPassphrase) {
        const bool wrong = res.status == ServerImport::Status::WrongPassphrase;
        bool ok = false;
        passphrase = QInputDialog::getText(dialogParent(), tr("Import servers"),
                                           wrong ? tr("Wrong passphrase. Try again:")
                                                 : tr("This file is encrypted. Enter the passphrase used when exporting:"),
                                           QLineEdit::Password, QString(), &ok);
        if (!ok)
            return;
        res = ServerImport::load(path, passphrase);
    }
    if (res.status != ServerImport::Status::Ok) {
        Log::write(QStringLiteral("[import] failed: %1").arg(res.error.isEmpty() ? QStringLiteral("unknown") : res.error));
        QMessageBox::warning(dialogParent(), tr("Import servers"),
                             tr("This file could not be read.") + QStringLiteral("\n\n")
                                 + tr("Details are in the log file (Settings > Detailed logging > Open log file)."));
        return;
    }

    // Keys of what is already in the list, so importing twice does not duplicate servers.
    QHash<QString, ServerProfile> known;
    const auto keyOf = [](const ServerProfile &p) {
        return QStringLiteral("%1:%2|%3").arg(p.host.trimmed().toLower()).arg(p.port).arg(p.user.trimmed().toLower());
    };
    for (const ServerProfile &p : m_store->all())
        known.insert(keyOf(p), p);

    int added = 0, skipped = res.unusable, filled = 0;
    QList<ServerProfile> toAdd;
    QList<ServerProfile> toUpdate;
    for (ServerProfile &p : res.servers) {
        const QString key = keyOf(p);
        if (known.contains(key)) {
            // Already in the list: it only takes the password it is missing.
            ServerProfile have = known.value(key);
            if (have.password.isEmpty() && !p.password.isEmpty()) {
                have.password = p.password;
                have.savePassword = true;
                toUpdate.append(have);
                known.insert(key, have);
                ++filled;
            }
            ++skipped;
            continue;
        }
        known.insert(key, p);
        toAdd.append(std::move(p));
        ++added;
    }
    m_store->importBatch(std::move(toAdd), toUpdate);
    Log::write(QStringLiteral("[import] %1 added, %2 skipped, %3 got a password").arg(added).arg(skipped).arg(filled));
    audit(ServerProfile{}, QStringLiteral("Import"), tr("%1 added, %2 skipped").arg(added).arg(skipped));
    if (added > 0 || filled > 0) {
        m_monitor->refreshAll();
        m_monitor->resolveCountries();
    }
    QMessageBox::information(dialogParent(), tr("Import servers"),
                             skipped > 0 ? tr("%1 servers imported, %2 already in the list or without an address.").arg(added).arg(skipped)
                                         : tr("%1 servers imported.").arg(added));
}

void Workspace::exportServers()
{
    const QList<ServerProfile> servers = m_store->all();
    if (servers.isEmpty()) {
        QMessageBox::information(dialogParent(), tr("Export servers"), tr("There are no servers to export."));
        return;
    }
    // A passphrase encrypts the file and lets it carry the saved passwords; without one the file holds none.
    QString passphrase;
    for (;;) {
        bool ok = false;
        passphrase = QInputDialog::getText(dialogParent(), tr("Export servers"),
                                           tr("Passphrase to protect the file and the saved passwords. Leave it empty to "
                                              "export the servers without passwords:"),
                                           QLineEdit::Password, QString(), &ok);
        if (!ok)
            return;
        if (passphrase.isEmpty())
            break;
        const QString again = QInputDialog::getText(dialogParent(), tr("Export servers"), tr("Type the passphrase again:"),
                                                    QLineEdit::Password, QString(), &ok);
        if (!ok)
            return;
        if (again == passphrase)
            break;
        QMessageBox::warning(dialogParent(), tr("Export servers"), tr("The passphrases are different."));
    }
    const QString name = QStringLiteral("rconnect-servers-%1.json")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd")));
    const QString path = QFileDialog::getSaveFileName(dialogParent(), tr("Export servers"), name, tr("Server list (*.json)"));
    if (path.isEmpty())
        return;
    if (!ServerImport::save(path, servers, passphrase)) {
        QMessageBox::warning(dialogParent(), tr("Export servers"), tr("Could not write the file."));
        return;
    }
    QMessageBox::information(dialogParent(), tr("Export servers"),
                             passphrase.isEmpty() ? tr("%1 servers exported, without passwords.").arg(servers.size())
                                                  : tr("%1 servers exported with their passwords.").arg(servers.size()));
}

void Workspace::editServer(const QString &id)
{
    const ServerProfile cur = m_store->profileById(id);
    if (!cur.isValid())
        return;
    ServerEditDialog dlg(cur, false, m_store->groups(), dialogParent());
    if (dlg.exec() != QDialog::Accepted)
        return;
    const ServerProfile updated = dlg.profile();
    {
        // Which settings changed (never the values of the password or notes).
        QStringList changed;
        const auto note = [&changed](bool differs, const char *name) { if (differs) changed << QLatin1String(name); };
        note(cur.name != updated.name, "name");
        note(cur.host != updated.host, "host");
        note(cur.port != updated.port, "port");
        note(cur.user != updated.user, "user");
        note(cur.domain != updated.domain, "domain");
        note(cur.password != updated.password, "password");
        note(cur.notes != updated.notes, "notes");
        note(cur.resolution != updated.resolution || cur.colorDepth != updated.colorDepth, "display");
        note(cur.clipboard != updated.clipboard || cur.drives != updated.drives || cur.audio != updated.audio,
             "redirection");
        note(cur.monitor != updated.monitor || cur.checkSessions != updated.checkSessions, "monitoring");
        audit(updated, QStringLiteral("ServerEdited"), changed.join(QStringLiteral(", ")));
    }
    m_store->update(updated);
    updateJumpList();
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
    audit(p, QStringLiteral("ServerRemoved"), QStringLiteral("%1:%2").arg(p.host).arg(p.port));
    m_store->remove(id);
    updateJumpList();
}

void Workspace::onSessionState(RdpSession::State state)
{
    auto *s = qobject_cast<RdpSession *>(sender());
    if (!s || m_sessions.value(s->serverId()) != s)
        return;
    const QString id = s->serverId();
    const bool localConnected = state == RdpSession::Connected;
    if (m_store->setLocalConnection(id, localConnected, localConnected ? s->connectedUser() : QString()))
        m_monitor->localConnectionChanged(id);
    SessionWindow *w = windowOf(s->serverId());
    if (w) {
        w->updateSessionTab(s, state == RdpSession::Replaced);
        if (state == RdpSession::Replaced)
            QApplication::alert(w, 0);
    }
    if ((state == RdpSession::Replaced || state == RdpSession::Disconnected) && !s->hasServerView())
        explainEnd(s);
    refreshOpenMarkers();
    updateLiveSessions();
}

// With a session open, RConnect is never slowed down in the background (see Priority).
void Workspace::updateLiveSessions()
{
    bool live = false;
    for (const RdpSession *s : std::as_const(m_sessions))
        live |= s->state() == RdpSession::Connected || s->state() == RdpSession::Connecting;
    Priority::setLiveSessions(live);
}

// The server does not always say why it ended a connection, and "another computer connected" is only what the
// control reports. Ask the server who is connected right now and tell that, instead of leaving a guess on screen.
void Workspace::explainEnd(RdpSession *s, int attempt)
{
    const ServerProfile p = m_store->profileById(s->serverId());
    const RdpSession::State ended = s->state();
    if (!p.isValid() || !StatusMonitor::canCheckSessions(p)) {
        if (ended == RdpSession::Replaced)
            notifyReplaced(s, s->serverName(), tr("Another computer connected to this server."));
        return;
    }
    if (attempt == 0)
        s->setCheckingServer(true);
    QPointer<RdpSession> session = s;
    m_monitor->checkSessions(p, [this, session, ended, attempt, name = p.name](const ServerStatus &status) {
        // Only if the tab is still showing that same ending.
        if (m_quitting || !session || session->state() != ended)
            return;
        if (status.occupancy == ServerStatus::OccupancyError) {
            session->setCheckingServer(false);
            if (ended == RdpSession::Replaced)
                notifyReplaced(session, name, tr("Another computer connected to this server."));
            return;
        }
        if (status.occupancy == ServerStatus::Busy && !status.others.isEmpty()) {
            QStringList whoLines;
            QStringList whoNames;
            for (const RemoteSession &r : status.others) {
                const QString label = Contacts::instance().label(r.client, r.user);
                const QString owner = label.isEmpty() ? sessionOwner(r) : label;
                whoNames << owner;
                whoLines << tr("%1 · user %2 · %3").arg(owner, r.user, idleText(r.idleMinutes));
            }
            const QString primary = sessionOwner(status.others.first());
            const QString title = status.others.size() == 1
                ? tr("%1 connected").arg(primary)
                : tr("%1 and %2 more connected").arg(primary).arg(status.others.size() - 1);
            if (ended == RdpSession::Replaced || session->wasUnknownServerClose()) {
                session->setServerView(
                    title,
                    tr("Your connection to “%1” ended because another user connected:\n\n%2\n\nReconnecting may disconnect them.")
                        .arg(name, whoLines.join(QLatin1Char('\n'))),
                    true);
                notifyReplaced(session, name, tr("%1 connected to this server.").arg(whoNames.join(QStringLiteral(", "))));
            } else {
                session->setServerView({}, tr("Connected right now:\n%1").arg(whoLines.join(QLatin1Char('\n'))));
            }
        } else if (status.mine) {
            session->setServerView(
                tr("Connected from another window on this PC"),
                tr("Another Remote Desktop connection from this computer took over “%1”.").arg(name),
                ended == RdpSession::Replaced || session->wasUnknownServerClose());
        } else if (attempt == 0 && (ended == RdpSession::Replaced || session->wasUnknownServerClose())) {
            // Windows disconnects the outgoing session before the incoming one finishes appearing as Active;
            // ask once more shortly before concluding nobody else is connected.
            QTimer::singleShot(2500, this, [this, session, ended] {
                if (!m_quitting && session && session->state() == ended)
                    explainEnd(session, 1);
            });
        } else if (ended == RdpSession::Replaced) {
            session->setServerView(tr("Disconnected by the server"),
                                   tr("Checked just now: nobody else is connected. The server ended this connection "
                                      "(a time limit, a restart or a network drop can do this)."));
            notifyReplaced(session, name, tr("Disconnected by the server."));
        } else {
            session->setServerView({}, tr("Checked just now: nobody else is connected."));
        }
    });
}

void Workspace::notifyReplaced(RdpSession *s, const QString &serverName, const QString &detail)
{
    if (!s || m_quitting)
        return;
    SessionWindow *w = windowOf(s->serverId());
    if (w && w->isActiveWindow() && w->currentSession() == s && s->isVisible())
        return; // the user is already looking at this tab's overlay
    m_lastTrayServerId = s->serverId();
    tray()->showMessage(tr("%1 — disconnected").arg(serverName), detail, QSystemTrayIcon::Warning, 15000);
}

void Workspace::refreshOpenMarkers()
{
    QSet<QString> open;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it)
        open.insert(it.key());
    m_home->panel()->setOpenSessionIds(open);
    for (QFrame *popup : std::as_const(m_popups))
        popup->findChild<ServerPanel *>()->setOpenSessionIds(open);
}

void Workspace::recordRecentServer(const QString &serverId)
{
    if (serverId.isEmpty())
        return;
    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("recentServers")).toStringList();
    recent.removeAll(serverId);
    recent.prepend(serverId);
    while (recent.size() > 8)
        recent.removeLast();
    settings.setValue(QStringLiteral("recentServers"), recent);
    updateJumpList();
}

void Workspace::updateJumpList()
{
    QSettings settings;
    const QStringList stored = settings.value(QStringLiteral("recentServers")).toStringList();
    QStringList cleaned;
    QList<JumpList::Item> items;
    for (const QString &id : stored) {
        const ServerProfile p = m_store->profileById(id);
        if (!p.isValid() || cleaned.contains(id))
            continue;
        cleaned << id;
        items << JumpList::Item{p.id, p.name, p.host};
        if (items.size() >= 8)
            break;
    }
    if (cleaned != stored)
        settings.setValue(QStringLiteral("recentServers"), cleaned);
    JumpList::update(tr("Recent servers"), items);
}

void Workspace::sendMessage(const QString &id, int sessionId, const QString &user)
{
    const ServerProfile p = m_store->profileById(id);
    if (!p.isValid() || sessionId <= 0 || !StatusMonitor::canCheckSessions(p))
        return;

    const auto activeSessions = m_store->status(id).all;
    const auto recipient = std::find_if(activeSessions.cbegin(), activeSessions.cend(), [sessionId, &user](const RemoteSession &s) {
        return s.id == sessionId && s.connected && s.user.compare(user, Qt::CaseInsensitive) == 0;
    });
    if (recipient == activeSessions.cend()) {
        QMessageBox::warning(dialogParent(), tr("Send message"), tr("This session is no longer connected."));
        return;
    }
    const QString client = Contacts::instance().label(recipient->client, user);
    const QString display = client.isEmpty() ? user : QStringLiteral("%1 · %2").arg(client, user);

    constexpr qsizetype kMessageLimit = 1024;
    QString text;
    while (true) {
        bool accepted = false;
        text = QInputDialog::getMultiLineText(
            dialogParent(), tr("Send message"),
            tr("Message for %1 on “%2” (up to %3 characters):").arg(display, p.name).arg(kMessageLimit),
            text, &accepted).trimmed();
        if (!accepted || text.isEmpty())
            return;
        if (text.size() <= kMessageLimit)
            break;
        QMessageBox::warning(dialogParent(), tr("Send message"), tr("Use at most %1 characters.").arg(kMessageLimit));
    }
    const ServerProfile current = m_store->profileById(id);
    if (!current.isValid() || !current.sameConnection(p) || !StatusMonitor::canCheckSessions(current)) {
        QMessageBox::warning(dialogParent(), tr("Send message"), tr("The server settings changed. Open the message again."));
        return;
    }

    QFile queueFile(QStringLiteral(":/message/Queue.ps1"));
    QFile popupFile(QStringLiteral(":/message/Show.ps1"));
    if (!queueFile.open(QIODevice::ReadOnly) || !popupFile.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(dialogParent(), tr("Send message"), tr("The message window resources could not be opened."));
        return;
    }

    const auto encodeText = [](const QString &value) { return QString::fromLatin1(value.toUtf8().toBase64()); };
    const QString encodedUser = encodeText(user);
    const QString popup = QString::fromUtf8(popupFile.readAll()).arg(sessionId).arg(
        encodedUser, encodeText(tr("Message from the administrator")), encodeText(text),
        encodeText(tr("Close")), encodeText(tr("Copy")));
    const QString taskName = QStringLiteral("RConnectMessage-") + QUuid::createUuid().toString(QUuid::Id128);
    const QString script = QString::fromUtf8(queueFile.readAll()).arg(sessionId).arg(encodedUser, popup.trimmed(), taskName);
    QPointer<Workspace> self(this);
    QThreadPool::globalInstance()->start([self, p, user, display, taskName, script] {
        Threats::expectMessageTask(p.id, taskName);
        const SessionQuery reply = WinRm::runPowerShell(p.host, p.user, p.domain, p.password, 12000, script);
        bool queued = false;
        QString error = reply.error;
        if (reply.result == SessionQuery::Ok) {
            const QByteArray errorPrefix = "RCONNECT_MESSAGE_ERROR ";
            for (const QByteArray &line : reply.output.split('\n')) {
                const QByteArray result = line.trimmed();
                if (result == "RCONNECT_MESSAGE_QUEUED")
                    queued = true;
                else if (result.startsWith(errorPrefix))
                    error = QString::fromUtf8(QByteArray::fromBase64(result.mid(errorPrefix.size())));
            }
            if (!queued && error.isEmpty())
                error = QString::fromUtf8(reply.output).simplified();
        }
        if (queued)
            AccessLog::note(p, QStringLiteral("Message"), p.displayUser(), user);
        QMetaObject::invokeMethod(qApp, [self, p, display, queued, error] {
            if (!self || self->m_quitting)
                return;
            auto *box = new QMessageBox(queued ? QMessageBox::Information : QMessageBox::Warning,
                                        tr("Send message"), {}, QMessageBox::Ok, self->dialogParent());
            box->setTextFormat(Qt::PlainText);
            box->setText(queued ? tr("Message queued for %1.").arg(display)
                                : tr("Could not verify that the message was queued on “%1”.").arg(p.name));
            if (!queued && !error.isEmpty())
                box->setInformativeText(error.left(550));
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->open();
        }, Qt::QueuedConnection);
    }, 1);
}

namespace {

constexpr const char *kHighPerfPowerGuid = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";
constexpr const char *kBalancedPowerGuid = "381b4222-f694-41f0-9685-ff5bb260df2e";

struct AuditSubcategorySpec {
    const char *guid;
    int requiredMask; // 1 = Success, 3 = Success and Failure
    QString label;
};

struct AuditGroupSpec {
    QList<AuditSubcategorySpec> items;
};

struct LiveServerProbe {
    bool reachable = false;
    bool isAdmin = false;
    bool auditOk = false;
    bool hasIdleLimit = false;
    int currentIdleMin = 0;
    bool fps60Enabled = false;
    bool avc444Enabled = false;
    bool udpRulePresent = false;
    bool highPerfActive = false;
    bool highPerfAvailable = false;
    bool balancedActive = false;
    bool balancedAvailable = false;
    QHash<QString, int> auditMasks; // uppercase {GUID} -> bitmask (0..3)
    QString error;
};

struct ServerApplyPlan {
    ServerProfile profile;
    QString command;
    QString summary;
    QString logDetail;
};

QStringList splitCsvRow(const QString &line)
{
    QStringList cols;
    QString cur;
    bool inQuotes = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('"')) {
            if (inQuotes && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                cur.append(QLatin1Char('"'));
                ++i;
            } else {
                inQuotes = !inQuotes;
            }
        } else if (ch == QLatin1Char(',') && !inQuotes) {
            cols << cur.trimmed();
            cur.clear();
        } else {
            cur.append(ch);
        }
    }
    cols << cur.trimmed();
    return cols;
}

int parseAuditSettingMask(const QStringList &cols)
{
    if (cols.size() >= 7) {
        bool ok = false;
        const int v = cols.at(6).toInt(&ok);
        if (ok && v >= 0 && v <= 3)
            return v;
    }
    const QString inc = cols.value(4).toLower();
    if (inc.contains(QLatin1String("no auditing")) || inc.contains(QLatin1String("denetleme yok"))
        || inc.contains(QLatin1String("denetim yok")))
        return 0;
    int mask = 0;
    if (inc.contains(QLatin1String("failure")) || inc.contains(QLatin1String("başarısız"))
        || inc.contains(QLatin1String("hata")))
        mask |= 2;
    QString withoutFail = inc;
    withoutFail.remove(QLatin1String("başarısızlık")).remove(QLatin1String("başarısız"));
    if (withoutFail.contains(QLatin1String("success")) || withoutFail.contains(QLatin1String("başarı")))
        mask |= 1;
    return mask;
}

std::optional<qint64> parseRegDword(const QString &section, const QString &valueName)
{
    const QRegularExpression re(QStringLiteral("%1\\s+REG_DWORD\\s+(0x[0-9a-fA-F]+|\\d+)").arg(QRegularExpression::escape(valueName)));
    const auto m = re.match(section);
    if (!m.hasMatch())
        return std::nullopt;
    bool ok = false;
    const QString val = m.captured(1);
    const qint64 num = val.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)
        ? val.mid(2).toLongLong(&ok, 16)
        : val.toLongLong(&ok, 10);
    return ok ? std::optional<qint64>(num) : std::nullopt;
}

QString sectionBetween(const QString &text, QLatin1String startMarker, QLatin1String endMarker)
{
    const qsizetype s = text.indexOf(startMarker);
    if (s < 0)
        return {};
    const qsizetype from = s + startMarker.size();
    const qsizetype e = endMarker.isEmpty() ? -1 : text.indexOf(endMarker, from);
    return e < 0 ? text.mid(from) : text.mid(from, e - from);
}

LiveServerProbe parseVerifyReply(const SessionQuery &reply)
{
    LiveServerProbe probe;
    if (reply.result != SessionQuery::Ok) {
        probe.error = reply.error;
        return probe;
    }
    const QString out = QString::fromUtf8(reply.output);
    if (!out.contains(QLatin1String("##RCVERIFYEND##"))) {
        probe.error = reply.error.isEmpty() ? out.simplified().left(180) : reply.error;
        return probe;
    }
    probe.reachable = true;
    probe.isAdmin = out.contains(QLatin1String("##RCADMIN:1##"));
    probe.auditOk = out.contains(QLatin1String("##RCAUDITOK##")) && !out.contains(QLatin1String("##RCAUDITDENIED##"));

    const qsizetype idlePos = out.indexOf(QLatin1String("##RCIDLE##"));
    const QString auditPart = idlePos >= 0 ? out.left(idlePos) : out;

    if (probe.auditOk) {
        const QStringList lines = auditPart.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &rawLine : lines) {
            const QString line = rawLine.trimmed();
            if (!line.contains(QLatin1Char('{')) || !line.contains(QLatin1Char('}')))
                continue;
            const QStringList cols = splitCsvRow(line);
            for (int i = 0; i < cols.size(); ++i) {
                const QString &col = cols.at(i);
                if (col.startsWith(QLatin1Char('{')) && col.endsWith(QLatin1Char('}')) && col.size() == 38) {
                    probe.auditMasks.insert(col.toUpper(), parseAuditSettingMask(cols));
                    break;
                }
            }
        }
    }

    const QString idlePart = sectionBetween(out, QLatin1String("##RCIDLE##"), QLatin1String("##RCFPS##"));
    if (!idlePart.contains(QLatin1String("##RCIDLENONE##"))) {
        if (const auto ms = parseRegDword(idlePart, QStringLiteral("MaxIdleTime")); ms && *ms > 0) {
            probe.hasIdleLimit = true;
            probe.currentIdleMin = int(qMax<qint64>(1, (*ms + 30000) / 60000));
        }
    }

    const QString fpsPart = sectionBetween(out, QLatin1String("##RCFPS##"), QLatin1String("##RCAVC##"));
    if (!fpsPart.contains(QLatin1String("##RCFPSNONE##"))) {
        if (const auto interval = parseRegDword(fpsPart, QStringLiteral("DWMFRAMEINTERVAL")); interval && *interval > 0 && *interval <= 15)
            probe.fps60Enabled = true;
    }

    const QString avcPart = sectionBetween(out, QLatin1String("##RCAVC##"), QLatin1String("##RCUDP##"));
    if (!avcPart.contains(QLatin1String("##RCAVCNONE##"))) {
        if (const auto avc = parseRegDword(avcPart, QStringLiteral("AVC444ModePreferred")); avc && *avc == 1)
            probe.avc444Enabled = true;
    }

    const QString udpPart = sectionBetween(out, QLatin1String("##RCUDP##"), QLatin1String("##RCPWR##"));
    probe.udpRulePresent = udpPart.contains(QLatin1String("##RCUDPON##"));

    const QString pwrActive = sectionBetween(out, QLatin1String("##RCPWR##"), QLatin1String("##RCPWRLIST##"));
    const QString pwrList = sectionBetween(out, QLatin1String("##RCPWRLIST##"), QLatin1String("##RCVERIFYEND##"));
    probe.highPerfActive = pwrActive.contains(QLatin1String(kHighPerfPowerGuid), Qt::CaseInsensitive);
    probe.balancedActive = pwrActive.contains(QLatin1String(kBalancedPowerGuid), Qt::CaseInsensitive);
    probe.highPerfAvailable = pwrList.contains(QLatin1String(kHighPerfPowerGuid), Qt::CaseInsensitive);
    probe.balancedAvailable = pwrList.contains(QLatin1String(kBalancedPowerGuid), Qt::CaseInsensitive);
    return probe;
}

} // namespace

void Workspace::configureServerLogging()
{
    QList<ServerProfile> targets;
    for (const ServerProfile &p : m_store->all())
        if (StatusMonitor::canCheckSessions(p))
            targets << p;
    if (targets.isEmpty()) {
        QMessageBox::information(dialogParent(), tr("Configure servers"),
            tr("No servers have WinRM session checks enabled."));
        return;
    }

    const QList<AuditGroupSpec> auditGroups = {
        {{{ "{0CCE921C-69AE-11D9-BED3-505054503030}", 1, tr("Computer name") }}},
        {{{ "{0CCE9215-69AE-11D9-BED3-505054503030}", 3, tr("Sign-ins") },
          { "{0CCE9217-69AE-11D9-BED3-505054503030}", 3, tr("Lockouts") }}},
        {{{ "{0CCE9235-69AE-11D9-BED3-505054503030}", 3, tr("Accounts/passwords") },
          { "{0CCE9237-69AE-11D9-BED3-505054503030}", 1, tr("Admin groups") },
          { "{0CCE922F-69AE-11D9-BED3-505054503030}", 1, tr("Audit policy") }}},
        {{{ "{0CCE9227-69AE-11D9-BED3-505054503030}", 1, tr("Background tasks") }}},
    };

    QDialog dlg(dialogParent());
    dlg.setWindowTitle(tr("Configure servers"));
    dlg.setMinimumSize(820, 560);
    dlg.resize(880, 600);

    // Matches SettingsDialog chrome: left rail (#settingsRail) + scrollable main pane + footer (#settingsFooter).
    auto *outer = new QVBoxLayout(&dlg);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    outer->addLayout(body, 1);

    // ---- Left rail: server picker with search ----
    auto *rail = new QFrame(&dlg);
    rail->setObjectName(QStringLiteral("settingsRail"));
    rail->setFixedWidth(250);
    auto *railLay = new QVBoxLayout(rail);
    railLay->setContentsMargins(14, 20, 14, 14);
    railLay->setSpacing(10);

    auto *railHead = new QHBoxLayout;
    railHead->setSpacing(6);
    auto *srvHeading = new QLabel(tr("Target servers"), rail);
    srvHeading->setObjectName(QStringLiteral("settingsHeading"));
    auto *selCount = new QLabel(rail);
    selCount->setObjectName(QStringLiteral("fieldHint"));
    railHead->addWidget(srvHeading);
    railHead->addStretch(1);
    railHead->addWidget(selCount);
    railLay->addLayout(railHead);

    auto *searchEdit = new QLineEdit(rail);
    searchEdit->setPlaceholderText(tr("Filter servers…"));
    searchEdit->setClearButtonEnabled(true);
    railLay->addWidget(searchEdit);

    auto *pickBtns = new QHBoxLayout;
    pickBtns->setSpacing(6);
    auto *selectAllBtn = new QToolButton(rail);
    selectAllBtn->setObjectName(QStringLiteral("sectionToggle"));
    selectAllBtn->setText(tr("Select all"));
    selectAllBtn->setCursor(Qt::PointingHandCursor);
    auto *clearSelBtn = new QToolButton(rail);
    clearSelBtn->setObjectName(QStringLiteral("sectionToggle"));
    clearSelBtn->setText(tr("Clear"));
    clearSelBtn->setCursor(Qt::PointingHandCursor);
    pickBtns->addWidget(selectAllBtn);
    pickBtns->addWidget(clearSelBtn);
    pickBtns->addStretch(1);
    railLay->addLayout(pickBtns);

    auto *serverList = new QListWidget(rail);
    serverList->setObjectName(QStringLiteral("settingsNavigation"));
    serverList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    serverList->setSelectionMode(QAbstractItemView::NoSelection);
    for (int i = 0; i < targets.size(); ++i) {
        const ServerProfile &p = targets.at(i);
        auto *item = new QListWidgetItem(p.name, serverList);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setData(Qt::UserRole, i);
        item->setToolTip(p.host);
    }
    railLay->addWidget(serverList, 1);
    body->addWidget(rail);

    // ---- Right main pane ----
    auto *rightPane = new QWidget(&dlg);
    auto *rightOuter = new QVBoxLayout(rightPane);
    rightOuter->setContentsMargins(26, 22, 26, 16);
    rightOuter->setSpacing(14);

    auto *pageTitle = new QLabel(tr("Server configuration"), rightPane);
    pageTitle->setObjectName(QStringLiteral("settingsTitle"));
    auto *pageSub = new QLabel(
        tr("Adjusts built-in Windows logging and Remote Desktop smoothness only on checked servers. "
           "Nothing is installed and TCP RDP access is never altered."),
        rightPane);
    pageSub->setObjectName(QStringLiteral("fieldHint"));
    pageSub->setWordWrap(true);
    rightOuter->addWidget(pageTitle);
    rightOuter->addWidget(pageSub);

    // Clean segmented tab bar matching ServerPanel (#segmented / #segment).
    auto *segFrame = new QFrame(rightPane);
    segFrame->setObjectName(QStringLiteral("segmented"));
    auto *segLay = new QHBoxLayout(segFrame);
    segLay->setContentsMargins(4, 4, 4, 4);
    segLay->setSpacing(2);
    auto *tabGroup = new QButtonGroup(&dlg);
    tabGroup->setExclusive(true);
    auto *tabLog = new QPushButton(tr("Logging & security"), segFrame);
    auto *tabPerf = new QPushButton(tr("Connection smoothness"), segFrame);
    auto *tabVerify = new QPushButton(tr("Live verification"), segFrame);
    QPushButton *tabButtons[3] = {tabLog, tabPerf, tabVerify};
    for (int i = 0; i < 3; ++i) {
        tabButtons[i]->setObjectName(QStringLiteral("segment"));
        tabButtons[i]->setCheckable(true);
        tabButtons[i]->setAutoDefault(false);
        tabButtons[i]->setCursor(Qt::PointingHandCursor);
        tabGroup->addButton(tabButtons[i], i);
        segLay->addWidget(tabButtons[i]);
    }
    tabLog->setChecked(true);
    auto *segRow = new QHBoxLayout;
    segRow->addWidget(segFrame);
    segRow->addStretch(1);
    rightOuter->addLayout(segRow);

    auto *stack = new QStackedWidget(rightPane);
    rightOuter->addWidget(stack, 1);
    connect(tabGroup, &QButtonGroup::idClicked, stack, &QStackedWidget::setCurrentIndex);

    const auto makeScrollTab = [&]() -> QVBoxLayout * {
        auto *scroll = new QScrollArea(stack);
        scroll->setObjectName(QStringLiteral("settingsScroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *content = new QWidget;
        content->setObjectName(QStringLiteral("settingsContent"));
        auto *lay = new QVBoxLayout(content);
        lay->setContentsMargins(0, 4, 4, 8);
        lay->setSpacing(14);
        scroll->setWidget(content);
        stack->addWidget(scroll);
        return lay;
    };
    const auto makeSection = [](QVBoxLayout *parent) -> QVBoxLayout * {
        auto *frame = new QFrame;
        frame->setObjectName(QStringLiteral("settingsSection"));
        auto *lay = new QVBoxLayout(frame);
        lay->setContentsMargins(16, 14, 16, 14);
        lay->setSpacing(12);
        parent->addWidget(frame);
        return lay;
    };
    const auto addAlignedComboRow = [](QVBoxLayout *sec, const QString &title, const QString &hint,
                                       const QList<QPair<QString, int>> &opts) -> QComboBox * {
        auto *row = new QHBoxLayout;
        row->setSpacing(14);
        auto *textCol = new QVBoxLayout;
        textCol->setSpacing(2);
        auto *lbl = new QLabel(title);
        auto *sub = new QLabel(hint);
        sub->setObjectName(QStringLiteral("fieldHint"));
        sub->setWordWrap(true);
        textCol->addWidget(lbl);
        textCol->addWidget(sub);
        auto *combo = new QComboBox;
        combo->setFixedWidth(220);
        for (const auto &[text, val] : opts)
            combo->addItem(text, val);
        row->addLayout(textCol, 1);
        row->addWidget(combo, 0, Qt::AlignVCenter);
        sec->addLayout(row);
        return combo;
    };

    // -- Tab 0: Logging & Security --
    QVBoxLayout *logTab = makeScrollTab();
    QVBoxLayout *auditSec = makeSection(logTab);
    QList<QCheckBox *> checks;
    const QList<QPair<QString, QString>> groupLabels = {
        {tr("Record connecting computer name"),
         tr("Shows which PC connected even when users share the same IP address.")},
        {tr("Record sign-in attempts and account lockouts"),
         tr("Logs failed password attempts and locked accounts.")},
        {tr("Record user, password and admin group changes"),
         tr("Logs account creation/deletion, password changes, and admin permission edits.")},
        {tr("Record new or edited background tasks"),
         tr("Logs when a Windows scheduled task is created, edited, or removed.")},
    };
    for (const auto &[text, hint] : groupLabels) {
        auto *itemBox = new QVBoxLayout;
        itemBox->setSpacing(2);
        auto *cb = new QCheckBox(text);
        cb->setChecked(false);
        auto *sub = new QLabel(hint);
        sub->setObjectName(QStringLiteral("fieldHint"));
        sub->setWordWrap(true);
        sub->setContentsMargins(24, 0, 0, 0);
        itemBox->addWidget(cb);
        itemBox->addWidget(sub);
        auditSec->addLayout(itemBox);
        checks << cb;
    }

    QVBoxLayout *idleSec = makeSection(logTab);
    QComboBox *idleCombo = addAlignedComboRow(
        idleSec,
        tr("Disconnect idle sessions"),
        tr("Ends unattended RDP sessions automatically after inactivity; open apps keep running."),
        {{tr("Leave unchanged"), -1},
         {tr("15 minutes (recommended)"), 15},
         {tr("10 minutes"), 10},
         {tr("20 minutes"), 20},
         {tr("30 minutes"), 30},
         {tr("Disable (no limit)"), 0}});
    logTab->addStretch(1);

    // -- Tab 1: Connection Smoothness (reversible) --
    QVBoxLayout *perfTab = makeScrollTab();
    QVBoxLayout *perfSec = makeSection(perfTab);
    QComboBox *fpsCombo = addAlignedComboRow(
        perfSec,
        tr("Display smoothness (FPS)"),
        tr("Raises RDP from 30 FPS to 60 FPS. Best for low-latency servers with enough CPU."),
        {{tr("Leave unchanged"), -1}, {tr("Enable 60 FPS"), 1}, {tr("Revert to 30 FPS"), 0}});
    QComboBox *avcCombo = addAlignedComboRow(
        perfSec,
        tr("Smooth video encoding (H.264)"),
        tr("Uses H.264 / AVC 444 for sharper scrolling and lower bandwidth usage."),
        {{tr("Leave unchanged"), -1}, {tr("Enable H.264"), 1}, {tr("Revert to default"), 0}});
    QComboBox *udpCombo = addAlignedComboRow(
        perfSec,
        tr("Fast UDP transport"),
        tr("Allows UDP on the server's RDP port in Windows Firewall for smoother high-latency links."),
        {{tr("Leave unchanged"), -1}, {tr("Allow UDP on RDP port"), 1}, {tr("Remove RConnect rule"), 0}});
    QComboBox *powerCombo = addAlignedComboRow(
        perfSec,
        tr("Server power plan"),
        tr("Prevents CPU frequency drops. Skipped automatically if unsupported on a VM."),
        {{tr("Leave unchanged"), -1}, {tr("High Performance"), 1}, {tr("Balanced (default)"), 0}});

    auto *perfNote = new QLabel(
        tr("Changes take effect on the next reconnect. Every option above can be reverted to stock Windows defaults from this same screen."),
        rightPane);
    perfNote->setObjectName(QStringLiteral("fieldHint"));
    perfNote->setWordWrap(true);
    perfTab->addWidget(perfNote);
    perfTab->addStretch(1);

    // -- Tab 2: Live Verification & Apply Results (clean per-server cards, no table grid) --
    QVBoxLayout *verifyTab = makeScrollTab();
    auto *verifyListHost = new QWidget;
    auto *verifyCardsLay = new QVBoxLayout(verifyListHost);
    verifyCardsLay->setContentsMargins(0, 0, 0, 0);
    verifyCardsLay->setSpacing(10);
    verifyTab->addWidget(verifyListHost);
    verifyTab->addStretch(1);

    body->addWidget(rightPane, 1);

    // ---- Footer bar matching SettingsDialog (#settingsFooter) ----
    auto *footer = new QFrame(&dlg);
    footer->setObjectName(QStringLiteral("settingsFooter"));
    auto *footLay = new QHBoxLayout(footer);
    footLay->setContentsMargins(20, 10, 20, 10);
    footLay->setSpacing(8);
    auto *summaryLabel = new QLabel(footer);
    summaryLabel->setObjectName(QStringLiteral("fieldHint"));
    summaryLabel->setWordWrap(true);
    auto *closeBtn = new QPushButton(tr("Close"), footer);
    closeBtn->setAutoDefault(false);
    closeBtn->setCursor(Qt::PointingHandCursor);
    auto *verifyBtn = new QPushButton(tr("Verify live"), footer);
    verifyBtn->setAutoDefault(false);
    verifyBtn->setCursor(Qt::PointingHandCursor);
    auto *applyBtn = new QPushButton(tr("Apply…"), footer);
    applyBtn->setObjectName(QStringLiteral("primaryButton"));
    applyBtn->setAutoDefault(false);
    applyBtn->setCursor(Qt::PointingHandCursor);
    applyBtn->setEnabled(false);
    footLay->addWidget(summaryLabel, 1);
    footLay->addWidget(closeBtn);
    footLay->addWidget(verifyBtn);
    footLay->addWidget(applyBtn);
    outer->addWidget(footer);

    // ---- State & live verification / apply engine ----
    struct ApplyState {
        enum Kind { None, Running, Done, Failed } kind = None;
        QString detail;
    };
    QHash<QString, LiveServerProbe> probes;
    QHash<QString, ApplyState> applyStates;
    QList<ServerApplyPlan> readyPlans;
    bool verifying = false;
    bool applying = false;
    bool bulkUpdating = false;
    auto opEpoch = std::make_shared<std::atomic_int>(0);

    const auto selectedRows = [&] {
        QList<int> rows;
        for (int i = 0; i < serverList->count(); ++i)
            if (serverList->item(i)->checkState() == Qt::Checked)
                rows << i;
        return rows;
    };

    const auto selectedOptionCount = [&] {
        int n = 0;
        for (QCheckBox *cb : checks)
            if (cb->isChecked())
                ++n;
        for (QComboBox *combo : {idleCombo, fpsCombo, avcCombo, udpCombo, powerCombo})
            if (combo->currentData().toInt() >= 0)
                ++n;
        return n;
    };

    const auto buildPlan = [&](const ServerProfile &p, const LiveServerProbe &probe,
                               QString &statusText, QColor &statusColor, QString &detailText) -> std::optional<ServerApplyPlan> {
        if (!probe.reachable) {
            statusText = tr("Unreachable");
            statusColor = Theme::kRed;
            detailText = probe.error.isEmpty() ? tr("Could not reach server over WinRM.") : probe.error;
            return std::nullopt;
        }
        const bool wantAudit = std::any_of(checks.cbegin(), checks.cend(), [](QCheckBox *cb) { return cb->isChecked(); });
        const int idleMinutes = idleCombo->currentData().toInt();
        const int fpsMode = fpsCombo->currentData().toInt();
        const int avcMode = avcCombo->currentData().toInt();
        const int udpMode = udpCombo->currentData().toInt();
        const int powerMode = powerCombo->currentData().toInt();
        const bool wantAdminChange = idleMinutes >= 0 || fpsMode >= 0 || avcMode >= 0 || udpMode >= 0 || powerMode >= 0;

        if ((wantAudit && (!probe.isAdmin || !probe.auditOk)) || (wantAdminChange && !probe.isAdmin)) {
            statusText = tr("No permission");
            statusColor = Theme::kRed;
            detailText = !probe.isAdmin
                ? tr("Account lacks elevated Administrator permission on this server.")
                : tr("Account is not allowed to query/modify Windows audit policy (auditpol).");
            return std::nullopt;
        }

        QStringList cmds;
        QStringList missing;
        QStringList alreadyOn;
        QStringList unsupported;

        for (int g = 0; g < checks.size(); ++g) {
            if (!checks.at(g)->isChecked())
                continue;
            for (const AuditSubcategorySpec &sub : auditGroups.at(g).items) {
                const QString guidKey = QString::fromLatin1(sub.guid).toUpper();
                const int curMask = probe.auditMasks.value(guidKey, -1);
                if (curMask >= 0 && (curMask & sub.requiredMask) == sub.requiredMask) {
                    alreadyOn << sub.label;
                } else {
                    const QString flags = sub.requiredMask == 3
                        ? QStringLiteral("/success:enable /failure:enable")
                        : QStringLiteral("/success:enable");
                    cmds << QStringLiteral("auditpol /set /subcategory:\"%1\" %2").arg(QString::fromLatin1(sub.guid), flags);
                    missing << sub.label;
                }
            }
        }
        if (idleMinutes >= 0) {
            if (idleMinutes > 0) {
                if (probe.hasIdleLimit && probe.currentIdleMin == idleMinutes) {
                    alreadyOn << tr("Idle %1 min").arg(idleMinutes);
                } else {
                    cmds << QStringLiteral("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows NT\\Terminal Services\" "
                                           "/v MaxIdleTime /t REG_DWORD /d %1 /f")
                                .arg(qint64(idleMinutes) * 60000);
                    missing << (probe.hasIdleLimit
                                    ? tr("Idle %1→%2 min").arg(probe.currentIdleMin).arg(idleMinutes)
                                    : tr("Idle off→%1 min").arg(idleMinutes));
                }
            } else {
                if (!probe.hasIdleLimit) {
                    alreadyOn << tr("Idle off");
                } else {
                    cmds << QStringLiteral("(reg delete \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows NT\\Terminal Services\" "
                                           "/v MaxIdleTime /f 2>nul || exit /b 0)");
                    missing << tr("Idle %1 min→off").arg(probe.currentIdleMin);
                }
            }
        }
        if (fpsMode == 1) {
            if (probe.fps60Enabled) {
                alreadyOn << tr("60 FPS");
            } else {
                cmds << QStringLiteral("reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Terminal Server\\WinStations\" "
                                       "/v DWMFRAMEINTERVAL /t REG_DWORD /d 15 /f");
                missing << tr("60 FPS on");
            }
        } else if (fpsMode == 0) {
            if (!probe.fps60Enabled) {
                alreadyOn << tr("30 FPS default");
            } else {
                cmds << QStringLiteral("(reg delete \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Terminal Server\\WinStations\" "
                                       "/v DWMFRAMEINTERVAL /f 2>nul || exit /b 0)");
                missing << tr("60 FPS→30 FPS");
            }
        }
        if (avcMode == 1) {
            if (probe.avc444Enabled) {
                alreadyOn << tr("H.264");
            } else {
                cmds << QStringLiteral("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows NT\\Terminal Services\" "
                                       "/v AVC444ModePreferred /t REG_DWORD /d 1 /f");
                cmds << QStringLiteral("reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows NT\\Terminal Services\" "
                                       "/v AVCHardwareEncodePreferred /t REG_DWORD /d 1 /f");
                missing << tr("H.264 on");
            }
        } else if (avcMode == 0) {
            if (!probe.avc444Enabled) {
                alreadyOn << tr("H.264 default");
            } else {
                cmds << QStringLiteral("(reg delete \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows NT\\Terminal Services\" "
                                       "/v AVC444ModePreferred /f 2>nul || exit /b 0)");
                cmds << QStringLiteral("(reg delete \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows NT\\Terminal Services\" "
                                       "/v AVCHardwareEncodePreferred /f 2>nul || exit /b 0)");
                missing << tr("H.264→default");
            }
        }
        const int rdpPort = qBound(1, p.port, 65535);
        const QString ruleName = QStringLiteral("RConnect RDP UDP %1").arg(rdpPort);
        if (udpMode == 1) {
            if (probe.udpRulePresent) {
                alreadyOn << tr("UDP %1").arg(rdpPort);
            } else {
                cmds << QStringLiteral("netsh advfirewall firewall add rule name=\"%1\" dir=in action=allow protocol=UDP localport=%2")
                            .arg(ruleName).arg(rdpPort);
                missing << tr("UDP %1 on").arg(rdpPort);
            }
        } else if (udpMode == 0) {
            if (!probe.udpRulePresent) {
                alreadyOn << tr("No UDP rule");
            } else {
                cmds << QStringLiteral("(netsh advfirewall firewall delete rule name=\"%1\" >nul 2>&1 || exit /b 0)")
                            .arg(ruleName);
                missing << tr("Remove UDP %1 rule").arg(rdpPort);
            }
        }
        if (powerMode == 1) {
            if (probe.highPerfActive) {
                alreadyOn << tr("High Performance");
            } else if (!probe.highPerfAvailable) {
                unsupported << tr("High Performance unavailable on this VM");
            } else {
                cmds << QStringLiteral("powercfg /setactive %1").arg(QLatin1String(kHighPerfPowerGuid));
                missing << tr("High Performance");
            }
        } else if (powerMode == 0) {
            if (probe.balancedActive) {
                alreadyOn << tr("Balanced");
            } else if (!probe.balancedAvailable) {
                unsupported << tr("Balanced plan unavailable on this VM");
            } else {
                cmds << QStringLiteral("powercfg /setactive %1").arg(QLatin1String(kBalancedPowerGuid));
                missing << tr("Balanced power");
            }
        }

        if (cmds.isEmpty()) {
            statusText = tr("No change needed");
            statusColor = Theme::kGreen;
            QStringList notes;
            if (!alreadyOn.isEmpty())
                notes << tr("Already configured (%1)").arg(alreadyOn.join(QStringLiteral(", ")));
            if (!unsupported.isEmpty())
                notes << unsupported.join(QStringLiteral(", "));
            detailText = notes.join(QStringLiteral(" · "));
            return std::nullopt;
        }
        statusText = tr("Needs update");
        statusColor = Theme::kBlueText;
        detailText = tr("Will enable/update: %1").arg(missing.join(QStringLiteral(", ")));
        if (!alreadyOn.isEmpty())
            detailText += tr(" · Already on: %1").arg(alreadyOn.join(QStringLiteral(", ")));
        if (!unsupported.isEmpty())
            detailText += QStringLiteral(" · ") + unsupported.join(QStringLiteral(", "));

        ServerApplyPlan plan;
        plan.profile = p;
        plan.command = cmds.join(QStringLiteral(" && ")) + QStringLiteral(" && echo ##RCOK##");
        plan.summary = missing.join(QStringLiteral(", "));
        plan.logDetail = missing.join(QStringLiteral(", "));
        return plan;
    };

    const auto rebuildVerifyCards = [&] {
        while (QLayoutItem *it = verifyCardsLay->takeAt(0)) {
            delete it->widget();
            delete it;
        }
        const QList<int> rows = selectedRows();
        const int optCount = selectedOptionCount();
        selCount->setText(QStringLiteral("%1 / %2").arg(rows.size()).arg(targets.size()));
        tabVerify->setText(rows.isEmpty() ? tr("Live verification") : tr("Live verification (%1)").arg(rows.size()));
        readyPlans.clear();

        int unverifiedSelected = 0;
        int alreadyOkCount = 0;
        int blockedCount = 0;

        if (rows.isEmpty() || optCount == 0) {
            auto *emptyCard = new QFrame(verifyListHost);
            emptyCard->setObjectName(QStringLiteral("settingsSection"));
            auto *elay = new QVBoxLayout(emptyCard);
            elay->setContentsMargins(16, 18, 16, 18);
            auto *msg = new QLabel(
                rows.isEmpty()
                    ? tr("Select one or more servers from the list on the left to begin.")
                    : tr("Choose at least one option under Logging or Connection smoothness, then click “Verify live”."),
                emptyCard);
            msg->setObjectName(QStringLiteral("fieldHint"));
            msg->setWordWrap(true);
            elay->addWidget(msg);
            verifyCardsLay->addWidget(emptyCard);
        }

        for (int i = 0; i < targets.size(); ++i) {
            const ServerProfile &p = targets.at(i);
            QListWidgetItem *listItem = serverList->item(i);
            const bool checked = listItem->checkState() == Qt::Checked;
            if (!checked) {
                listItem->setText(p.name);
                continue;
            }
            if (optCount == 0) {
                listItem->setText(p.name);
                continue;
            }

            QString stText, dtText;
            QColor stColor = Theme::kLabelSecondary;
            QString railTag;
            const ApplyState ap = applyStates.value(p.id);
            if (ap.kind == ApplyState::Running) {
                stText = tr("Applying…");
                stColor = Theme::kBlueText;
                dtText = tr("Sending verified changes to server…");
                railTag = QStringLiteral(" …");
            } else if (ap.kind == ApplyState::Done) {
                stText = tr("Applied");
                stColor = Theme::kGreen;
                dtText = ap.detail;
                railTag = QStringLiteral(" ✓");
                ++alreadyOkCount;
            } else if (ap.kind == ApplyState::Failed) {
                stText = tr("Apply failed");
                stColor = Theme::kRed;
                dtText = ap.detail;
                railTag = QStringLiteral(" !");
                ++blockedCount;
            } else if (!probes.contains(p.id)) {
                ++unverifiedSelected;
                stText = verifying ? tr("Verifying…") : tr("Not verified");
                stColor = Theme::kOrange;
                dtText = verifying ? tr("Checking live state & permissions…") : tr("Click “Verify live” to inspect this server.");
            } else {
                if (auto plan = buildPlan(p, probes.value(p.id), stText, stColor, dtText)) {
                    readyPlans << *plan;
                    railTag = QStringLiteral(" •");
                } else if (stColor == Theme::kGreen) {
                    railTag = QStringLiteral(" ✓");
                    ++alreadyOkCount;
                } else {
                    railTag = QStringLiteral(" !");
                    ++blockedCount;
                }
            }
            listItem->setText(p.name + railTag);

            auto *card = new QFrame(verifyListHost);
            card->setObjectName(QStringLiteral("settingsSection"));
            auto *clay = new QVBoxLayout(card);
            clay->setContentsMargins(14, 11, 14, 11);
            clay->setSpacing(4);
            auto *top = new QHBoxLayout;
            top->setSpacing(8);
            auto *nameLbl = new QLabel(p.name, card);
            nameLbl->setFont(Theme::font(13, QFont::DemiBold));
            auto *hostLbl = new QLabel(p.host, card);
            hostLbl->setObjectName(QStringLiteral("fieldHint"));
            auto *stLbl = new QLabel(stText, card);
            stLbl->setFont(Theme::font(12, QFont::DemiBold));
            stLbl->setStyleSheet(QStringLiteral("color: %1;").arg(stColor.name()));
            top->addWidget(nameLbl);
            top->addWidget(hostLbl);
            top->addStretch(1);
            top->addWidget(stLbl);
            auto *detLbl = new QLabel(dtText, card);
            detLbl->setObjectName(QStringLiteral("fieldHint"));
            detLbl->setWordWrap(true);
            clay->addLayout(top);
            clay->addWidget(detLbl);
            verifyCardsLay->addWidget(card);
        }

        const bool busy = verifying || applying;
        verifyBtn->setEnabled(!busy && !rows.isEmpty() && optCount > 0);
        applyBtn->setEnabled(!busy && !rows.isEmpty() && optCount > 0 && unverifiedSelected == 0 && !readyPlans.isEmpty());
        closeBtn->setEnabled(!applying);

        if (rows.isEmpty() || optCount == 0) {
            summaryLabel->setText(tr("Select servers and options, then click “Verify live”."));
        } else if (applying) {
            summaryLabel->setText(tr("Applying verified changes…"));
        } else if (verifying) {
            summaryLabel->setText(tr("Verifying %1 server(s) live (read-only)…").arg(rows.size()));
        } else if (unverifiedSelected > 0) {
            summaryLabel->setText(tr("%1 server(s) need live verification before Apply unlocks.").arg(unverifiedSelected));
        } else if (readyPlans.isEmpty()) {
            summaryLabel->setText(tr("Nothing to apply: %1 up to date, %2 blocked/unreachable.")
                                      .arg(alreadyOkCount).arg(blockedCount));
        } else {
            summaryLabel->setText(tr("Ready: %1 server(s) will be updated (%2 already up to date, %3 skipped).")
                                      .arg(readyPlans.size()).arg(alreadyOkCount).arg(blockedCount));
        }
    };

    const auto onSelectionOrOptionChanged = [&] {
        applyStates.clear();
        rebuildVerifyCards();
    };

    connect(searchEdit, &QLineEdit::textChanged, &dlg, [&](const QString &q) {
        const QString needle = q.trimmed().toLower();
        for (int i = 0; i < targets.size(); ++i) {
            const ServerProfile &p = targets.at(i);
            const bool match = needle.isEmpty()
                || p.name.toLower().contains(needle)
                || p.host.toLower().contains(needle)
                || p.group.toLower().contains(needle);
            serverList->item(i)->setHidden(!match);
        }
    });
    connect(selectAllBtn, &QToolButton::clicked, &dlg, [&] {
        bulkUpdating = true;
        for (int i = 0; i < serverList->count(); ++i)
            if (!serverList->item(i)->isHidden())
                serverList->item(i)->setCheckState(Qt::Checked);
        bulkUpdating = false;
        onSelectionOrOptionChanged();
    });
    connect(clearSelBtn, &QToolButton::clicked, &dlg, [&] {
        bulkUpdating = true;
        for (int i = 0; i < serverList->count(); ++i)
            serverList->item(i)->setCheckState(Qt::Unchecked);
        bulkUpdating = false;
        onSelectionOrOptionChanged();
    });
    connect(serverList, &QListWidget::itemChanged, &dlg, [&](QListWidgetItem *) {
        if (!bulkUpdating)
            onSelectionOrOptionChanged();
    });
    for (QCheckBox *cb : checks)
        connect(cb, &QCheckBox::toggled, &dlg, onSelectionOrOptionChanged);
    for (QComboBox *combo : {idleCombo, fpsCombo, avcCombo, udpCombo, powerCombo})
        connect(combo, &QComboBox::currentIndexChanged, &dlg, onSelectionOrOptionChanged);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    connect(verifyBtn, &QPushButton::clicked, &dlg, [&] {
        const QList<int> rows = selectedRows();
        if (rows.isEmpty() || selectedOptionCount() == 0 || verifying || applying)
            return;
        verifying = true;
        applyStates.clear();
        const int myEpoch = ++(*opEpoch);
        for (int r : rows)
            probes.remove(targets.at(r).id);
        tabVerify->setChecked(true);
        stack->setCurrentIndex(2);
        rebuildVerifyCards();

        auto remaining = std::make_shared<std::atomic_int>(int(rows.size()));
        QPointer<QDialog> guard(&dlg);
        for (int r : rows) {
            const ServerProfile p = targets.at(r);
            const int rdpPort = qBound(1, p.port, 65535);
            const QString verifyCmd = QStringLiteral(
                "(whoami /groups | findstr /C:\"S-1-16-12288\" /C:\"S-1-16-16384\" >nul 2>&1 && echo ##RCADMIN:1## || echo ##RCADMIN:0##) & "
                "(auditpol /get /category:* /r && echo ##RCAUDITOK## || echo ##RCAUDITDENIED##) & "
                "echo ##RCIDLE## & (reg query \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows NT\\Terminal Services\" /v MaxIdleTime 2>nul || echo ##RCIDLENONE##) & "
                "echo ##RCFPS## & (reg query \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Terminal Server\\WinStations\" /v DWMFRAMEINTERVAL 2>nul || echo ##RCFPSNONE##) & "
                "echo ##RCAVC## & (reg query \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows NT\\Terminal Services\" /v AVC444ModePreferred 2>nul || echo ##RCAVCNONE##) & "
                "echo ##RCUDP## & (netsh advfirewall firewall show rule name=\"RConnect RDP UDP %1\" >nul 2>&1 && echo ##RCUDPON## || echo ##RCUDPOFF##) & "
                "echo ##RCPWR## & (powercfg /getactivescheme 2>nul) & echo ##RCPWRLIST## & (powercfg /list 2>nul) & "
                "echo ##RCVERIFYEND##").arg(rdpPort);
            QThreadPool::globalInstance()->start([guard, p, verifyCmd, opEpoch, myEpoch, remaining, &probes, &verifying, &rebuildVerifyCards] {
                const SessionQuery reply = WinRm::runCommand(p.host, p.user, p.domain, p.password, 15000, verifyCmd);
                const LiveServerProbe parsed = parseVerifyReply(reply);
                const int left = --(*remaining);
                QMetaObject::invokeMethod(qApp, [guard, p, parsed, opEpoch, myEpoch, left, &probes, &verifying, &rebuildVerifyCards] {
                    if (!guard || opEpoch->load() != myEpoch)
                        return;
                    probes.insert(p.id, parsed);
                    if (left <= 0)
                        verifying = false;
                    rebuildVerifyCards();
                }, Qt::QueuedConnection);
            });
        }
    });

    connect(applyBtn, &QPushButton::clicked, &dlg, [&] {
        if (readyPlans.isEmpty() || verifying || applying)
            return;
        QDialog confirmDlg(&dlg);
        confirmDlg.setWindowTitle(tr("Confirm server changes"));
        confirmDlg.setMinimumWidth(500);
        auto *clay = new QVBoxLayout(&confirmDlg);
        clay->setContentsMargins(20, 18, 20, 16);
        clay->setSpacing(12);

        auto *warn = new QLabel(
            tr("Apply verified Windows settings on %1 server(s)? Servers that are already up to date or lack permission are skipped automatically:")
                .arg(readyPlans.size()),
            &confirmDlg);
        warn->setWordWrap(true);
        clay->addWidget(warn);

        QStringList previewLines;
        for (const ServerApplyPlan &plan : std::as_const(readyPlans))
            previewLines << QStringLiteral("• %1 (%2)\n  %3").arg(plan.profile.name, plan.profile.host, plan.summary);
        auto *preview = new QPlainTextEdit(previewLines.join(QStringLiteral("\n\n")), &confirmDlg);
        preview->setReadOnly(true);
        preview->setMaximumHeight(170);
        clay->addWidget(preview);

        const QString keyword = tr("APPLY");
        auto *typeLabel = new QLabel(tr("Type “%1” to confirm:").arg(keyword), &confirmDlg);
        auto *typeEdit = new QLineEdit(&confirmDlg);
        typeEdit->setPlaceholderText(keyword);
        clay->addWidget(typeLabel);
        clay->addWidget(typeEdit);

        auto *cBtns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &confirmDlg);
        QPushButton *okBtn = cBtns->button(QDialogButtonBox::Ok);
        okBtn->setText(tr("Apply now"));
        okBtn->setObjectName(QStringLiteral("primaryButton"));
        okBtn->setEnabled(false);
        connect(typeEdit, &QLineEdit::textChanged, &confirmDlg, [okBtn, keyword](const QString &t) {
            okBtn->setEnabled(t.trimmed().compare(keyword, Qt::CaseInsensitive) == 0);
        });
        connect(cBtns, &QDialogButtonBox::accepted, &confirmDlg, &QDialog::accept);
        connect(cBtns, &QDialogButtonBox::rejected, &confirmDlg, &QDialog::reject);
        clay->addWidget(cBtns);

        if (confirmDlg.exec() != QDialog::Accepted)
            return;

        const QList<ServerApplyPlan> plansToRun = readyPlans;
        applying = true;
        const int myEpoch = ++(*opEpoch);
        for (const ServerApplyPlan &plan : plansToRun)
            applyStates.insert(plan.profile.id, {ApplyState::Running, {}});
        tabVerify->setChecked(true);
        stack->setCurrentIndex(2);
        rebuildVerifyCards();

        auto remaining = std::make_shared<std::atomic_int>(int(plansToRun.size()));
        QPointer<QDialog> guard(&dlg);
        QPointer<Workspace> self(this);
        for (const ServerApplyPlan &plan : plansToRun) {
            QThreadPool::globalInstance()->start([guard, self, plan, opEpoch, myEpoch, remaining, &probes, &applyStates, &applying, &rebuildVerifyCards] {
                const ServerProfile &p = plan.profile;
                const SessionQuery reply = WinRm::runCommand(p.host, p.user, p.domain, p.password, 15000, plan.command);
                const bool succeeded = reply.result == SessionQuery::Ok && reply.output.contains("##RCOK##");
                QString err = reply.error;
                if (!succeeded && err.isEmpty())
                    err = QString::fromUtf8(reply.output).simplified().left(200);
                if (succeeded)
                    AccessLog::note(p, QStringLiteral("ServerConfigured"), p.displayUser(), plan.logDetail);
                const int left = --(*remaining);
                QMetaObject::invokeMethod(qApp, [guard, self, p, plan, succeeded, err, opEpoch, myEpoch, left, &probes, &applyStates, &applying, &rebuildVerifyCards] {
                    if (left <= 0 && self && !self->m_quitting)
                        self->m_monitor->refreshEvents({});
                    if (!guard || opEpoch->load() != myEpoch)
                        return;
                    if (succeeded) {
                        probes.remove(p.id);
                        applyStates.insert(p.id, {ApplyState::Done, plan.summary});
                    } else {
                        applyStates.insert(p.id, {ApplyState::Failed, err.isEmpty() ? tr("Command failed.") : err});
                    }
                    if (left <= 0)
                        applying = false;
                    rebuildVerifyCards();
                }, Qt::QueuedConnection);
            });
        }
    });

    rebuildVerifyCards();
    dlg.exec();
    ++(*opEpoch); // ignore late callbacks after closing
}

// A warning from Threats, or a tampering event, as a Windows notification.
QSystemTrayIcon *Workspace::tray()
{
    if (!m_tray) {
        m_tray = new QSystemTrayIcon(QApplication::windowIcon(), this);
        m_tray->setToolTip(QStringLiteral("RConnect"));
        connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this] {
            if (!m_lastTrayServerId.isEmpty()) {
                if (SessionWindow *w = windowOf(m_lastTrayServerId)) {
                    w->activateSession(m_lastTrayServerId);
                    return;
                }
            }
            if (m_main->isMinimized())
                m_main->showNormal();
            m_main->raise();
            m_main->activateWindow();
        });
    }
    m_tray->show();
    return m_tray;
}

void Workspace::showAlert(const AccessRecord &alert)
{
    m_lastTrayServerId.clear();
    tray()->showMessage(tr("Security alert: %1").arg(alert.server), AccessLogPage::alertSummary(alert),
                        QSystemTrayIcon::Warning, 20000);
}

void Workspace::showStranger(const QString &server, const QString &client, const QString &user)
{
    m_lastTrayServerId.clear();
    tray()->showMessage(tr("Unknown person on %1").arg(server),
                        tr("%1 (account %2) connected and is not in your contacts.").arg(client, user),
                        QSystemTrayIcon::Warning, 20000);
}

void Workspace::checkForUpdates(bool manual)
{
    if (m_updateChecking || m_updateDialogOpen)
        return;
    m_updateChecking = true;
    m_manualCheck = manual;
    m_updater->check();
}

void Workspace::onUpdateChecked(bool ok, bool available, const UpdateInfo &info, const QString &error)
{
    m_updateChecking = false;
    if (!ok) {
        if (m_manualCheck)
            QMessageBox::warning(dialogParent(), tr("Check for updates"),
                                 tr("Could not check for updates: %1").arg(error));
        return;
    }
    if (!available) {
        if (m_manualCheck)
            QMessageBox::information(dialogParent(), tr("Check for updates"),
                                     tr("RConnect %1 is the latest version.").arg(QApplication::applicationVersion()));
        return;
    }
    if (!m_manualCheck) {
        if (UpdateChecker::skippedVersion() == info.version)
            return;
        if (m_main->isMinimized()) {
            m_pendingUpdate = info;
            return;
        }
    }
    offerUpdate(info);
}

void Workspace::offerUpdate(const UpdateInfo &info)
{
    if (m_updateDialogOpen)
        return;
    m_updateDialogOpen = true;
    const bool canInstall = UpdateChecker::installedBySetup() && info.installerUrl.isValid();

    QMessageBox box(QMessageBox::Information, tr("Update available"),
                    tr("RConnect %1 is available. You have %2.").arg(info.version, QApplication::applicationVersion()),
                    QMessageBox::NoButton, dialogParent());
    if (!info.notes.isEmpty())
        box.setDetailedText(info.notes);
    QPushButton *update = box.addButton(canInstall ? tr("Update now") : tr("Open download page"), QMessageBox::AcceptRole);
    box.addButton(tr("Later"), QMessageBox::RejectRole);
    QPushButton *skip = box.addButton(tr("Skip this version"), QMessageBox::DestructiveRole);
    box.setDefaultButton(update);
    box.exec();
    m_updateDialogOpen = false;

    if (box.clickedButton() == update) {
        if (canInstall)
            installUpdate(info);
        else
            QDesktopServices::openUrl(info.pageUrl);
    } else if (box.clickedButton() == skip) {
        UpdateChecker::setSkippedVersion(info.version);
    }
}

void Workspace::installUpdate(const UpdateInfo &info)
{
    bool anyConnected = false;
    for (RdpSession *s : std::as_const(m_sessions))
        anyConnected |= s->state() == RdpSession::Connected;
    if (anyConnected
        && !confirm(dialogParent(), tr("Update"),
                    tr("Installing the update closes RConnect and all open connections. Continue?"), false))
        return;

    auto *progress = new QProgressDialog(tr("Downloading RConnect %1…").arg(info.version), tr("Cancel"), 0, 0,
                                         dialogParent());
    progress->setWindowTitle(tr("Update"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    connect(progress, &QProgressDialog::canceled, m_updater, &UpdateChecker::cancelDownload);
    connect(m_updater, &UpdateChecker::downloadProgress, progress, [progress](qint64 received, qint64 total) {
        if (total <= 0)
            return;
        progress->setMaximum(int(total / 1024));
        progress->setValue(int(received / 1024));
    });
    connect(m_updater, &UpdateChecker::downloadFailed, progress, [this, progress](const QString &error) {
        const bool cancelled = progress->wasCanceled();
        progress->close();
        if (cancelled)
            return;
        QMessageBox::warning(dialogParent(), tr("Update"), tr("The update could not be downloaded: %1").arg(error));
    });
    connect(m_updater, &UpdateChecker::downloaded, progress, [this, progress, info](const QString &path) {
        progress->close();
        // The installer closes this copy, installs over it and starts the new one.
        audit(ServerProfile{}, QStringLiteral("Update"), tr("Installing %1").arg(info.version));
        if (QProcess::startDetached(path, {QStringLiteral("/SILENT"), QStringLiteral("/CLOSEAPPLICATIONS")})) {
            quit(false);
        } else {
            QMessageBox::warning(dialogParent(), tr("Update"),
                                 tr("The installer could not be started. You can run it yourself:\n%1")
                                     .arg(QDir::toNativeSeparators(path)));
        }
    });
    progress->show();
    m_updater->download(info);
}
