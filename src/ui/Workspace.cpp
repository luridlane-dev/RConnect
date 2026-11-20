#include "Workspace.h"

#include "AccessLogPage.h"
#include "HomePage.h"
#include "ServerEditDialog.h"
#include "ServerPanel.h"
#include "SessionWindow.h"
#include "Theme.h"
#include "core/AccessLog.h"
#include "core/Contacts.h"
#include "core/Log.h"
#include "core/Priority.h"
#include "core/ServerImport.h"
#include "core/StatusMonitor.h"
#include "core/Threats.h"
#include "core/WinRm.h"
#include "model/ServerStore.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QRegularExpression>
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

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Theme::kPopupBorder, 1));
        p.setBrush(Theme::kPopupFill);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    }
};

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
    , m_updater(new UpdateChecker(this))
{
    m_connectLogPool.setMaxThreadCount(1);
    m_home = new HomePage(m_store);
    connect(&Contacts::instance(), &Contacts::changed, m_store, &ServerStore::refreshDisplay);
    connect(m_home, &HomePage::importRequested, this, &Workspace::importServers);
    connect(m_home, &HomePage::exportRequested, this, &Workspace::exportServers);
    connect(m_home, &HomePage::updateCheckRequested, this, [this] { checkForUpdates(true); });
    connect(m_updater, &UpdateChecker::checked, this, &Workspace::onUpdateChecked);
    connect(m_monitor, &StatusMonitor::alertRaised, this, &Workspace::showAlert);
    connect(m_home->accessLog(), &AccessLogPage::syncRequested, m_monitor, &StatusMonitor::refreshEvents);
    connect(m_monitor, &StatusMonitor::eventSyncChanged, m_home->accessLog(), &AccessLogPage::setSyncState);
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

void Workspace::start(bool startMinimized)
{
    m_store->load();
    m_main->restoreGeometry(QSettings().value(QStringLiteral("geometry")).toByteArray());
    if (startMinimized)
        m_main->showMinimized();
    else
        m_main->showFullScreen();
    // The "new country" check compares with the history; read it before the first events arrive.
    QThreadPool::globalInstance()->start([] {
        AccessLog::maintain(); // folds password-guessing runs and drops very old lines before anything reads the file
        Threats::warmUp();
    });
    auto *maintenance = new QTimer(this);
    connect(maintenance, &QTimer::timeout, this, [] { QThreadPool::globalInstance()->start([] { AccessLog::maintain(); }); });
    maintenance->start(24 * 3600 * 1000);
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
    reveal->start(900);

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
        popup = new PopupFrame(w, Qt::Popup | Qt::FramelessWindowHint);
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
    m_monitor->refreshAllNow(); // what the cards show may be a minute old
    popup->show();
    panel->focusSearch();
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
    if (Log::detailed() || event == QLatin1String("ServerAdded") || event == QLatin1String("Closed"))
        AccessLog::note(p, event, p.displayUser(), detail);
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
    for (ServerProfile p : res.servers) {
        const QString key = keyOf(p);
        if (known.contains(key)) {
            // Already in the list: it only takes the password it is missing.
            ServerProfile have = known.value(key);
            if (have.password.isEmpty() && !p.password.isEmpty()) {
                have.password = p.password;
                have.savePassword = true;
                m_store->update(have);
                known.insert(key, have);
                ++filled;
            }
            ++skipped;
            continue;
        }
        known.insert(key, p);
        m_store->add(p);
        ++added;
    }
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
    if (state == RdpSession::Replaced || state == RdpSession::Disconnected)
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
void Workspace::explainEnd(RdpSession *s)
{
    const ServerProfile p = m_store->profileById(s->serverId());
    if (!p.isValid() || !StatusMonitor::canCheckSessions(p))
        return;
    const RdpSession::State ended = s->state();
    QPointer<RdpSession> session = s;
    m_monitor->checkSessions(p, [this, session, ended, name = p.name](const ServerStatus &status) {
        // Only if the tab is still showing that same ending.
        if (m_quitting || !session || session->state() != ended || status.occupancy == ServerStatus::OccupancyError)
            return;
        if (status.occupancy == ServerStatus::Busy) {
            QStringList who;
            for (const RemoteSession &r : status.others)
                who << tr("%1 (user %2, %3)").arg(sessionOwner(r), r.user, idleText(r.idleMinutes));
            session->setServerView({}, tr("Checked just now. Connected: %1").arg(who.join(QStringLiteral(", "))));
        } else if (ended == RdpSession::Replaced) {
            session->setServerView(tr("Disconnected by the server"),
                                   tr("Checked just now: nobody else is connected. The server ended this connection "
                                      "(a time limit, a restart or a network drop can do this)."));
        } else {
            session->setServerView({}, tr("Checked just now: nobody else is connected."));
        }
    });
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

// A warning from Threats, or a tampering event, as a Windows notification.
QSystemTrayIcon *Workspace::tray()
{
    if (!m_tray) {
        m_tray = new QSystemTrayIcon(QApplication::windowIcon(), this);
        m_tray->setToolTip(QStringLiteral("RConnect"));
        connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this] {
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
    tray()->showMessage(tr("Security alert: %1").arg(alert.server), AccessLogPage::alertSummary(alert),
                        QSystemTrayIcon::Warning, 20000);
}

void Workspace::showStranger(const QString &server, const QString &client, const QString &user)
{
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
