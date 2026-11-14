#include "Workspace.h"

#include "AccessLogPage.h"
#include "HomePage.h"
#include "LiveSessionsPage.h"
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
#include <QVBoxLayout>

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
        p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
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
    m_home = new HomePage(m_store);
    connect(&Contacts::instance(), &Contacts::changed, m_store, &ServerStore::refreshDisplay);
    connect(m_home, &HomePage::importRequested, this, &Workspace::importServers);
    connect(m_home, &HomePage::exportRequested, this, &Workspace::exportServers);
    connect(m_home, &HomePage::updateCheckRequested, this, [this] { checkForUpdates(true); });
    connect(m_updater, &UpdateChecker::checked, this, &Workspace::onUpdateChecked);
    connect(m_home->liveSessions(), &LiveSessionsPage::refreshRequested, m_monitor, &StatusMonitor::refreshAllNow);
    connect(m_home->liveSessions(), &LiveSessionsPage::messageRequested, this, &Workspace::sendMessage);
    connect(m_monitor, &StatusMonitor::alertRaised, this, &Workspace::showAlert);
    connect(m_monitor, &StatusMonitor::unknownPerson, this, &Workspace::showStranger);
    ServerPanel *panel = m_home->panel();
    connect(panel, &ServerPanel::auditRequested, this, &Workspace::enableAuditing);
    connect(panel, &ServerPanel::displayTuneRequested, this, &Workspace::tuneDisplay);
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

    if (!StatusMonitor::canCheckSessions(p)) {
        connectServer(p, target);
        return;
    }
    // Ask the server who is connected right before connecting; a minute-old status is too old.
    if (m_pendingOpens.contains(id))
        return;
    m_pendingOpens.insert(id);
    QApplication::setOverrideCursor(Qt::BusyCursor);
    QPointer<SessionWindow> window = target;
    m_monitor->checkSessions(p, [this, p, window](const ServerStatus &status) {
        QApplication::restoreOverrideCursor();
        m_pendingOpens.remove(p.id);
        if (m_quitting || windowOf(p.id) || !m_store->profileById(p.id).isValid())
            return;
        SessionWindow *target = window ? window.data() : m_main;
        if (confirmBusy(p, status, target))
            connectServer(p, target);
    });
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
    AccessLog::note(p, QStringLiteral("Connect"), p.displayUser());
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
    updateLiveSessions();
    audit(m_store->profileById(id), QStringLiteral("Closed"));
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
        connect(panel, &ServerPanel::auditRequested, this, [this, popup](const QString &id) { popup->hide(); enableAuditing(id); });
        connect(panel, &ServerPanel::displayTuneRequested, this, [this, popup](const QString &id) { popup->hide(); tuneDisplay(id); });
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
    if (state == RdpSession::Replaced || state == RdpSession::Disconnected)
        explainEnd(s);
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
    QSet<QString> ids;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it)
        ids.insert(it.key());
    m_home->panel()->setOpenIds(ids);
    for (QFrame *popup : std::as_const(m_popups))
        popup->findChild<ServerPanel *>()->setOpenIds(ids);
}

// The Security log only has failed logons and disconnects if the server audits them.
void Workspace::enableAuditing(const QString &id)
{
    const ServerProfile p = m_store->profileById(id);
    if (!p.isValid())
        return;
    if (!StatusMonitor::canCheckSessions(p)) {
        QMessageBox::information(dialogParent(), tr("Track sign-in attempts"),
                                 tr("To do this, “%1” needs a saved administrator password and “Show who is connected” turned on in its settings.")
                                     .arg(p.name));
        return;
    }
    if (!confirm(dialogParent(), tr("Track sign-in attempts"),
                 tr("Record sign-in attempts on “%1”?\n\nThis changes a security setting on the server, so Windows keeps "
                    "a record of every sign-in and every failed attempt, and of new accounts, changes to the "
                    "administrators and new scheduled tasks. They show up in Activity history.")
                     .arg(p.name),
                 false))
        return;

    QApplication::setOverrideCursor(Qt::BusyCursor);
    QPointer<Workspace> self(this);
    QThreadPool::globalInstance()->start([self, p] {
        // Logon, Other Logon/Logoff Events, User Account Management, Security Group Management,
        // Other Object Access Events (scheduled tasks).
        QStringList steps;
        for (const char *guid : {"0CCE9215", "0CCE921C", "0CCE9235", "0CCE9237", "0CCE9227"})
            steps << QStringLiteral("auditpol /set /subcategory:\"{%1-69AE-11D9-BED3-505054503030}\" /success:enable "
                                    "/failure:enable").arg(QLatin1String(guid));
        const QString command = steps.join(QStringLiteral(" && ")) + QStringLiteral(" && echo ##OK## || echo ##FAILED##");
        const SessionQuery q = WinRm::runCommand(p.host, p.user, p.domain, p.password, 15000, command);
        const bool ok = q.result == SessionQuery::Ok && q.output.contains("##OK##");
        const QString error = q.result != SessionQuery::Ok ? q.error : QString::fromUtf8(q.output).simplified();
        QMetaObject::invokeMethod(qApp, [self, p, ok, error] {
            QApplication::restoreOverrideCursor();
            if (!self)
                return;
            if (ok) {
                AccessLog::note(p, QStringLiteral("AuditEnable"), p.displayUser());
                QMessageBox::information(self->dialogParent(), tr("Track sign-in attempts"),
                                         tr("Sign-in tracking is on for “%1”. New activity will appear in Activity history.").arg(p.name));
            } else {
                QMessageBox::warning(self->dialogParent(), tr("Track sign-in attempts"),
                                     tr("Could not change the setting on “%1”: %2").arg(p.name, error));
            }
        }, Qt::QueuedConnection);
    });
}

// Windows servers send at most 30 frames a second and, by default, colour-reduced (AVC 4:2:0) pictures, which make
// small text soft. Two documented server settings lift that: DWMFRAMEINTERVAL=15 (60 fps, Microsoft KB 2885213) and
// the policy "Prioritize H.264/AVC 444 graphics mode". The local monitor's refresh rate cannot be passed on: the
// picture changes only as often as the server sends it.
void Workspace::tuneDisplay(const QString &id)
{
    const ServerProfile p = m_store->profileById(id);
    if (!p.isValid())
        return;
    if (!StatusMonitor::canCheckSessions(p)) {
        QMessageBox::information(dialogParent(), tr("Smoother, sharper picture"),
                                 tr("To do this, “%1” needs a saved administrator password and “Show who is connected” turned on in its settings.")
                                     .arg(p.name));
        return;
    }
    if (!confirm(dialogParent(), tr("Smoother, sharper picture"),
                 tr("Change the picture settings of “%1”?\n\nThe server will send up to 60 frames a second instead of 30, "
                    "and text in full colour so it stays sharp. This changes two settings on the server and uses a "
                    "little more network. It takes effect after the server restarts.")
                     .arg(p.name),
                 false))
        return;

    QApplication::setOverrideCursor(Qt::BusyCursor);
    QPointer<Workspace> self(this);
    QThreadPool::globalInstance()->start([self, p] {
        const QString command = QStringLiteral(
            "reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Terminal Server\\WinStations\" /v DWMFRAMEINTERVAL "
            "/t REG_DWORD /d 15 /f && reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows NT\\Terminal Services\" "
            "/v AVC444ModePreferred /t REG_DWORD /d 1 /f && echo ##OK## || echo ##FAILED##");
        const SessionQuery q = WinRm::runCommand(p.host, p.user, p.domain, p.password, 15000, command);
        const bool ok = q.result == SessionQuery::Ok && q.output.contains("##OK##");
        const QString error = q.result != SessionQuery::Ok ? q.error : QString::fromUtf8(q.output).simplified();
        QMetaObject::invokeMethod(qApp, [self, p, ok, error] {
            QApplication::restoreOverrideCursor();
            if (!self)
                return;
            if (ok) {
                AccessLog::note(p, QStringLiteral("DisplayTuned"), p.displayUser());
                QMessageBox::information(self->dialogParent(), tr("Smoother, sharper picture"),
                                         tr("Done. Restart “%1” once for the new picture settings to take effect.").arg(p.name));
            } else {
                QMessageBox::warning(self->dialogParent(), tr("Smoother, sharper picture"),
                                     tr("Could not change the setting on “%1”: %2").arg(p.name, error));
            }
        }, Qt::QueuedConnection);
    });
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

// A pop-up on the user's screen in their session (msg.exe on the server).
void Workspace::sendMessage(const QString &serverId, int sessionId, const QString &user)
{
    const ServerProfile p = m_store->profileById(serverId);
    if (!p.isValid() || sessionId <= 0)
        return;
    constexpr int kMaxLength = 255;
    bool accepted = false;
    QString text = QInputDialog::getMultiLineText(
        dialogParent(), tr("Send message"), tr("Message for %1 on “%2” (up to %3 characters):").arg(user, p.name).arg(kMaxLength),
        {}, &accepted);
    text = text.trimmed();
    if (!accepted || text.isEmpty())
        return;
    text = text.left(kMaxLength);

    QApplication::setOverrideCursor(Qt::BusyCursor);
    QPointer<Workspace> self(this);
    const QString heading = tr("Message from the administrator");
    QThreadPool::globalInstance()->start([self, p, sessionId, user, text, heading] {
        // Text and title go through PowerShell as base64, so nothing in them is read as a command.
        const QString message = QString::fromLatin1(text.toUtf8().toBase64());
        const QString title = QString::fromLatin1(heading.toUtf8().toBase64());
        // A real warning box (yellow triangle, system sound, on top of everything) in the person's
        // session: WTSSendMessage with MB_ICONWARNING. msg.exe only shows plain text, so it stays as the
        // fallback if the call cannot be set up.
        // The message goes first. The box itself is silent when shown this way, so a short task then plays the system sound inside
        // the person's own session (the sound travels with their Remote Desktop audio, if they have it on).
        // The task runs as the signed-in user, interactively, and is removed again. A failure of the
        // sound never stops the message.
        const QString script = QStringLiteral(R"PS(
$m=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%1'))
$t=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%2'))
$sid=%3
$ex=0
try {
Add-Type -Namespace RC -Name W -MemberDefinition '[DllImport("wtsapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern bool WTSSendMessage(IntPtr h,int s,string t,int tl,string m,int ml,int st,int to,out int r,bool w);'
$r=0
if (-not [RC.W]::WTSSendMessage([IntPtr]::Zero,$sid,$t,$t.Length*2,$m,$m.Length*2,0x51030,300,[ref]$r,$false)) { $ex=[Runtime.InteropServices.Marshal]::GetLastWin32Error() }
} catch { msg.exe $sid /time:300 $m; $ex=$LASTEXITCODE }
try {
$o=Get-CimInstance Win32_Process -Filter "SessionId=$sid AND Name='explorer.exe'" | Select-Object -First 1 | Invoke-CimMethod -MethodName GetOwner
if ($ex -eq 0 -and $o -and $o.User) {
$dir=Join-Path $env:ProgramData 'RConnect'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$f=Join-Path $dir ('beep-'+[guid]::NewGuid().ToString('N')+'.ps1')
$snd='$w=@("Windows Notify Messaging.wav","Windows Notify System Generic.wav","Windows Exclamation.wav","Windows Ding.wav") | % { Join-Path $env:windir ("Media\"+$_) } | ? { Test-Path $_ } | Select-Object -First 1; if ($w) { (New-Object Media.SoundPlayer $w).PlaySync() }'
Set-Content -LiteralPath $f -Value $snd -Encoding ASCII
$n='RConnectBeep-'+[guid]::NewGuid().ToString('N')
$ps='-NoProfile -ExecutionPolicy Bypass -File "'+$f+'"'
if ([Environment]::OSVersion.Version.Build -ge 17763) { $a=New-ScheduledTaskAction -Execute 'conhost.exe' -Argument ('--headless powershell.exe '+$ps) }
else { $a=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-WindowStyle Hidden '+$ps) }
$pr=New-ScheduledTaskPrincipal -UserId ("$($o.Domain)\$($o.User)") -LogonType Interactive
Register-ScheduledTask -TaskName $n -Action $a -Principal $pr -Force | Out-Null
Start-ScheduledTask -TaskName $n
for ($i=0; $i -lt 40; $i++) { Start-Sleep -Milliseconds 250; if ((Get-ScheduledTask -TaskName $n).State -ne 'Running') { break } }
Unregister-ScheduledTask -TaskName $n -Confirm:$false
Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue
}
} catch { }
exit $ex
)PS").arg(message, title).arg(sessionId);
        const QByteArray encoded = QByteArray(reinterpret_cast<const char *>(script.utf16()), script.size() * 2).toBase64();
        const QString command = QStringLiteral("powershell -NoProfile -NonInteractive -EncodedCommand %1 "
                                               "&& echo ##OK## || echo ##FAILED##").arg(QString::fromLatin1(encoded));
        const SessionQuery q = WinRm::runCommand(p.host, p.user, p.domain, p.password, 30000, command);
        const bool ok = q.result == SessionQuery::Ok && q.output.contains("##OK##");
        const QString error = q.result != SessionQuery::Ok ? q.error : QString::fromUtf8(q.output).simplified();
        QMetaObject::invokeMethod(qApp, [self, p, user, text, ok, error] {
            QApplication::restoreOverrideCursor();
            if (!self)
                return;
            if (ok) {
                AccessLog::note(p, QStringLiteral("Message"), user, text.left(120));
                QMessageBox::information(self->dialogParent(), tr("Send message"),
                                         tr("The message was shown to %1.").arg(user));
            } else {
                QMessageBox::warning(self->dialogParent(), tr("Send message"),
                                     tr("Could not send the message on “%1”: %2").arg(p.name, error));
            }
        }, Qt::QueuedConnection);
    });
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
