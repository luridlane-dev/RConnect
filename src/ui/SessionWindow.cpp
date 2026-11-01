#include "SessionWindow.h"

#include "SessionTabs.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QScreen>
#include <QStackedWidget>
#include <QStyle>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

bool s_macButtons = false;
bool s_settingsRead = false; // the settings are read when the first window is made, after the application is named

QIcon homeIcon()
{
    constexpr qreal dpr = 2.0;
    QPixmap pm(int(16 * dpr), int(16 * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Theme::kIconLine, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath house;
    house.moveTo(2, 8);
    house.lineTo(8, 2.5);
    house.lineTo(14, 8);
    house.moveTo(3.5, 7);
    house.lineTo(3.5, 13.5);
    house.lineTo(12.5, 13.5);
    house.lineTo(12.5, 7);
    p.drawPath(house);
    return QIcon(pm);
}

QIcon dotIcon(const QColor &c)
{
    QPixmap pm(32, 32);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QRectF(4, 6, 8, 8));
    return QIcon(pm);
}

QColor sessionColor(RdpSession::State s)
{
    switch (s) {
    case RdpSession::Connected:    return Theme::kGreen;
    case RdpSession::Connecting:   return Theme::kBlue;
    case RdpSession::Replaced:     return Theme::kOrange;
    case RdpSession::Failed:
    case RdpSession::Disconnected: return Theme::kRed;
    default:                       return Theme::kLabelSecondary;
    }
}

} // namespace

SessionWindow::SessionWindow(QWidget *home, QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_home(home)
{
    setObjectName(QStringLiteral("root"));
    if (!s_settingsRead) {
        s_settingsRead = true;
        s_macButtons = QSettings().value(QStringLiteral("macWindowButtons"), false).toBool();
    }
    setAttribute(Qt::WA_DeleteOnClose, !isMain());
    resize(1280, 800);
    setMinimumSize(isMain() ? QSize(900, 560) : QSize(640, 400));

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_topBar = new QWidget(this);
    m_topBar->setObjectName(QStringLiteral("topBar"));
    m_topBar->setFixedHeight(AnimatedTabBar::kHeight);
    auto *bar = new QHBoxLayout(m_topBar);
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(0);

    // macOS style buttons, left; shown in full screen only.
    m_lights = new QWidget(m_topBar);
    auto *lightsLay = new QHBoxLayout(m_lights);
    lightsLay->setContentsMargins(14, 0, 6, 0);
    lightsLay->setSpacing(8);
    const char *const lightNames[] = {"trafficMin", "trafficMax", "trafficClose"};
    for (const char *name : lightNames) {
        auto *b = new QToolButton(m_lights);
        b->setObjectName(QLatin1String(name));
        b->setCursor(Qt::PointingHandCursor);
        m_lightButtons << b;
    }
    for (int i : {2, 0, 1}) // close first, as on a Mac
        lightsLay->addWidget(m_lightButtons[i]);
    m_lights->hide();
    bar->addWidget(m_lights);

    // The start page, always first and never closed.
    m_homeTab = new QToolButton(m_topBar);
    m_homeTab->setObjectName(QStringLiteral("homeTab"));
    m_homeTab->setIcon(homeIcon());
    m_homeTab->setIconSize(QSize(16, 16));
    m_homeTab->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_homeTab->setCheckable(true);
    m_homeTab->setChecked(true);
    m_homeTab->setCursor(Qt::PointingHandCursor);
    m_homeTab->setFixedHeight(AnimatedTabBar::kHeight);
    m_homeTab->setVisible(isMain());
    connect(m_homeTab, &QToolButton::clicked, this, &SessionWindow::showHome);
    bar->addWidget(m_homeTab);

    m_tabBar = new AnimatedTabBar(m_topBar);
    connect(m_tabBar, &QTabBar::currentChanged, this, &SessionWindow::onTabChanged);
    connect(m_tabBar, &QTabBar::tabBarClicked, this, [this] {
        m_homeActive = false;
        updatePage();
    });
    connect(m_tabBar, &AnimatedTabBar::addRequested, this, [this] { emit serversRequested(this); });
    connect(m_tabBar, &AnimatedTabBar::detachRequested, this, &SessionWindow::detachRequested);
    connect(m_tabBar, &AnimatedTabBar::tabDropped, this,
            [this](const QString &id, int index) { emit dropRequested(id, this, index); });
    m_tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabBar, &QWidget::customContextMenuRequested, this, &SessionWindow::showTabMenu);
    bar->addWidget(m_tabBar, 1);

    m_notesButton = new QToolButton(m_topBar);
    m_notesButton->setObjectName(QStringLiteral("barButton"));
    m_notesButton->setFixedHeight(AnimatedTabBar::kHeight);
    m_notesButton->setCursor(Qt::PointingHandCursor);
    m_notesButton->hide();
    connect(m_notesButton, &QToolButton::clicked, this, &SessionWindow::showNotes);
    bar->addWidget(m_notesButton);

    // Windows style buttons, right; shown in full screen only.
    m_winBox = new QWidget(m_topBar);
    auto *winLay = new QHBoxLayout(m_winBox);
    winLay->setContentsMargins(0, 0, 0, 0);
    winLay->setSpacing(0);
    const char *const glyphs[] = {"\u2013", "\u25a1", "\u00d7"};
    for (int i = 0; i < 3; ++i) {
        auto *b = new QToolButton(m_winBox);
        b->setObjectName(i == 2 ? QStringLiteral("winClose") : QStringLiteral("winButton"));
        b->setText(QString::fromUtf8(glyphs[i]));
        b->setFixedSize(46, AnimatedTabBar::kHeight);
        b->setCursor(Qt::PointingHandCursor);
        winLay->addWidget(b);
        m_winButtons << b;
    }
    m_winBox->hide();
    bar->addWidget(m_winBox);
    for (const QList<QToolButton *> &set : {m_winButtons, m_lightButtons}) {
        connect(set[0], &QToolButton::clicked, this, &QWidget::showMinimized);
        connect(set[1], &QToolButton::clicked, this, &SessionWindow::toggleFullscreen);
        connect(set[2], &QToolButton::clicked, this, &QWidget::close);
    }
    s_all << this;

    // The dot of a tab that is still connecting breathes.
    m_pulse.setInterval(50);
    connect(&m_pulse, &QTimer::timeout, this, &SessionWindow::pulseTabs);

    m_pages = new QStackedWidget(this);
    m_sessionStack = new QStackedWidget(m_pages);
    if (m_home)
        m_pages->addWidget(m_home);
    m_pages->addWidget(m_sessionStack);

    lay->addWidget(m_topBar);
    lay->addWidget(m_pages, 1);

    // Keys typed into a remote session go through the keyboard hook instead.
    auto bind = [this](const QKeySequence &k, auto fn) {
        auto *sc = new QShortcut(k, this);
        connect(sc, &QShortcut::activated, this, fn);
    };
    bind(QKeySequence(Qt::CTRL | Qt::Key_W), [this] {
        if (RdpSession *s = currentSession())
            emit closeTabRequested(s->serverId());
    });
    bind(QKeySequence(Qt::CTRL | Qt::Key_N), [this] { emit addServerRequested(); });
    bind(QKeySequence(Qt::CTRL | Qt::Key_B), [this] { m_topBar->setVisible(!m_topBar->isVisible()); });
    bind(QKeySequence(Qt::Key_F5), [this] { emit refreshStatusRequested(); });

    retranslate();
    updatePage();
    updateTitle();
    applyChrome();
    Theme::darkTitleBar(this); // also creates the native window
    connect(Theme::notifier(), &ThemeNotifier::changed, this, [this] {
        m_homeTab->setIcon(homeIcon());
        Theme::darkTitleBar(this);
        update();
    });
}

SessionWindow::~SessionWindow()
{
    s_all.removeAll(this);
}

QToolButton *SessionWindow::serversButton() const
{
    return m_tabBar->addButton();
}

int SessionWindow::tabIndexOf(const QString &serverId) const
{
    for (int i = 0; i < m_tabBar->count(); ++i)
        if (m_tabBar->tabData(i).toString() == serverId)
            return i;
    return -1;
}

RdpSession *SessionWindow::currentSession() const
{
    const int i = m_tabBar->currentIndex();
    return i < 0 ? nullptr : m_sessions.value(m_tabBar->tabData(i).toString());
}

void SessionWindow::addSession(RdpSession *s, int index)
{
    m_homeActive = false;
    const QString id = s->serverId();
    m_sessions.insert(id, s);
    m_sessionStack->addWidget(s);

    const int idx = m_tabBar->insertTab(index < 0 ? m_tabBar->count() : index,
                                        dotIcon(sessionColor(s->state())), s->serverName());
    m_tabBar->setTabData(idx, id);
    // The tab bar puts its button flush with the tab edge: the holder adds the right padding.
    auto *holder = new QWidget(m_tabBar);
    holder->setFixedSize(30, 20);
    auto *close = new QToolButton(holder);
    close->setObjectName(QStringLiteral("tabClose"));
    close->setText(QStringLiteral("×"));
    close->setToolTip(tr("Close tab (Ctrl+W)"));
    close->setCursor(Qt::PointingHandCursor);
    close->setFixedSize(18, 18);
    close->move(0, 0);
    connect(close, &QToolButton::clicked, this, [this, id] { emit closeTabRequested(id); });
    m_tabBar->setTabButton(idx, QTabBar::RightSide, holder);
    m_tabBar->setCurrentIndex(idx);
    onTabChanged(idx);
    updatePage();
    if (s->state() == RdpSession::Connecting)
        m_pulse.start();
}

RdpSession *SessionWindow::takeSession(const QString &serverId)
{
    RdpSession *s = m_sessions.take(serverId);
    if (!s)
        return nullptr;
    const int i = tabIndexOf(serverId);
    if (i >= 0)
        m_tabBar->removeTab(i);
    m_sessionStack->removeWidget(s);
    updatePage();
    updateNotesButton();
    updateTitle();
    return s;
}

void SessionWindow::activateSession(const QString &serverId)
{
    const int i = tabIndexOf(serverId);
    if (i >= 0)
        m_tabBar->setCurrentIndex(i);
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();
}

void SessionWindow::moveTab(const QString &serverId, int index)
{
    const int from = tabIndexOf(serverId);
    if (from < 0)
        return;
    const int to = qBound(0, index > from ? index - 1 : index, m_tabBar->count() - 1);
    if (to != from)
        m_tabBar->moveTab(from, to);
}

void SessionWindow::updateSessionTab(RdpSession *s, bool attention)
{
    const int i = tabIndexOf(s->serverId());
    if (i < 0)
        return;
    m_tabBar->setTabIcon(i, dotIcon(sessionColor(s->state())));
    if (s->state() == RdpSession::Connecting)
        m_pulse.start();
    const bool mark = attention && i != m_tabBar->currentIndex();
    m_tabBar->setTabText(i, mark ? s->serverName() + QStringLiteral("  !") : s->serverName());
    if (i == m_tabBar->currentIndex())
        updateTitle();
}

void SessionWindow::pulseTabs()
{
    const qreal phase = 0.5 + 0.5 * std::sin(QTime::currentTime().msecsSinceStartOfDay() / 1000.0 * 5.0);
    bool any = false;
    for (RdpSession *s : std::as_const(m_sessions)) {
        if (s->state() != RdpSession::Connecting)
            continue;
        const int i = tabIndexOf(s->serverId());
        if (i < 0)
            continue;
        any = true;
        QColor c = Theme::kBlue;
        c.setAlphaF(0.35 + 0.65 * phase);
        m_tabBar->setTabIcon(i, dotIcon(c));
    }
    if (!any)
        m_pulse.stop();
}

void SessionWindow::onTabChanged(int index)
{
    if (index < 0)
        return;
    m_homeActive = false;
    if (RdpSession *s = m_sessions.value(m_tabBar->tabData(index).toString())) {
        m_sessionStack->setCurrentWidget(s);
        m_tabBar->setTabText(index, s->serverName()); // clears the "!" marker
        s->focusRemote();
    }
    updateNotesButton();
    updateTitle();
}

void SessionWindow::showTabMenu(const QPoint &pos)
{
    const int i = m_tabBar->tabAt(pos);
    if (i < 0)
        return;
    const QString id = m_tabBar->tabData(i).toString();
    QMenu menu(this);
    menu.addAction(tr("Refresh connection"), this, [this, id] {
        if (RdpSession *s = m_sessions.value(id))
            s->refresh();
    });
    menu.addSeparator();
    if (!isMain() && sessionCount() > 0)
        menu.addAction(tr("Move to main window"), this, [this, id] { emit moveToMainRequested(id); });
    if (isMain() || sessionCount() > 1)
        menu.addAction(tr("Move to new window"), this, [this, id] {
            emit detachRequested(id, mapToGlobal(QPoint(width() / 4, height() / 4)));
        });
    menu.addSeparator();
    menu.addAction(tr("Close tab"), this, [this, id] { emit closeTabRequested(id); });
    menu.exec(m_tabBar->mapToGlobal(pos));
}

void SessionWindow::setNotesProvider(std::function<QString(const QString &)> provider)
{
    m_notes = std::move(provider);
    updateNotesButton();
}

void SessionWindow::updateNotesButton()
{
    RdpSession *s = currentSession();
    m_notesButton->setVisible(s && m_notes && !m_notes(s->serverId()).isEmpty());
}

void SessionWindow::showNotes()
{
    RdpSession *s = currentSession();
    if (!s || !m_notes)
        return;
    const QString text = m_notes(s->serverId());

    // Popup, so it shows above the native RDP window.
    auto *popup = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
    popup->setObjectName(QStringLiteral("popup"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *lay = new QVBoxLayout(popup);
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(8);
    auto *title = new QLabel(s->serverName(), popup);
    title->setObjectName(QStringLiteral("popupTitle"));
    auto *body = new QPlainTextEdit(text, popup);
    body->setReadOnly(true);
    body->setMinimumSize(340, 120);
    auto *copy = new QPushButton(tr("Copy"), popup);
    copy->setCursor(Qt::PointingHandCursor);
    connect(copy, &QPushButton::clicked, popup, [popup, body] {
        QString selected = body->textCursor().selectedText();
        selected.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        QApplication::clipboard()->setText(selected.isEmpty() ? body->toPlainText() : selected);
        popup->close();
    });
    auto *tip = new QLabel(tr("Select text to copy part of it."), popup);
    tip->setObjectName(QStringLiteral("fieldHint"));
    auto *row = new QHBoxLayout;
    row->addWidget(tip, 1);
    row->addWidget(copy);
    lay->addWidget(title);
    lay->addWidget(body, 1);
    lay->addLayout(row);

    popup->resize(380, 230);
    const QPoint anchor = m_notesButton->mapToGlobal(QPoint(m_notesButton->width(), m_notesButton->height() + 2));
    popup->move(anchor.x() - popup->width(), anchor.y());
    popup->show();
}

void SessionWindow::updatePage()
{
    if (!m_home)
        return;
    const bool home = m_homeActive || m_tabBar->count() == 0;
    m_pages->setCurrentIndex(home ? 0 : 1);
    m_homeTab->setChecked(home);
    m_tabBar->setSelectionVisible(!home);
    if (m_tabBar->count() == 0)
        m_homeActive = true;
}

void SessionWindow::showHome()
{
    m_homeActive = true;
    updatePage();
}

void SessionWindow::applyChrome()
{
    const bool full = isFullScreen();
    m_lights->setVisible(full && s_macButtons);
    m_winBox->setVisible(full && !s_macButtons);
}

void SessionWindow::setMacButtons(bool mac)
{
    s_macButtons = mac;
    QSettings().setValue(QStringLiteral("macWindowButtons"), mac);
    for (SessionWindow *w : std::as_const(s_all))
        w->applyChrome();
}

bool SessionWindow::macButtons()
{
    return s_macButtons;
}

void SessionWindow::updateTitle()
{
    RdpSession *s = currentSession();
    setWindowTitle(isMain() || !s ? QStringLiteral("RConnect")
                                  : QStringLiteral("%1 – RConnect").arg(s->serverName()));
}

void SessionWindow::retranslate()
{
    m_notesButton->setText(tr("Notes"));
    m_notesButton->setToolTip(tr("Notes for this server"));
    m_homeTab->setToolTip(tr("Home"));
    for (const QList<QToolButton *> &set : {m_winButtons, m_lightButtons}) {
        set[0]->setToolTip(tr("Minimise"));
        set[1]->setToolTip(tr("Exit full screen (F11)"));
        set[2]->setToolTip(tr("Close"));
    }
    for (int i = 0; i < m_tabBar->count(); ++i)
        if (QWidget *b = m_tabBar->tabButton(i, QTabBar::RightSide))
            if (QToolButton *close = b->findChild<QToolButton *>())
                close->setToolTip(tr("Close tab (Ctrl+W)"));
}

void SessionWindow::handleHotkey(Hotkey key, int arg)
{
    const int n = m_tabBar->count();
    switch (key) {
    case NextTab:
        if (n > 1)
            m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + 1) % n);
        break;
    case PrevTab:
        if (n > 1)
            m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + n - 1) % n);
        break;
    case GoToTab:
        if (arg < n)
            m_tabBar->setCurrentIndex(arg);
        break;
    case ToggleFullscreen:
        toggleFullscreen();
        break;
    }
}

void SessionWindow::toggleFullscreen()
{
    if (!isFullScreen()) {
        showFullScreen();
        return;
    }
    showNormal();
    // The normal geometry may still be the whole screen (e.g. first run): fall back to a window.
    const QRect avail = screen()->availableGeometry();
    if (width() >= avail.width() || height() >= avail.height()) {
        const QSize size(avail.width() * 3 / 4, avail.height() * 3 / 4);
        setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, size, avail));
    }
}

void SessionWindow::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange) {
        applyChrome();
        emit minimizedChanged(isMinimized());
    } else if (e->type() == QEvent::LanguageChange) {
        retranslate();
    }
}

void SessionWindow::closeEvent(QCloseEvent *e)
{
    if (!m_allowClose) {
        e->ignore();
        emit closeRequested(this);
        return;
    }
    QWidget::closeEvent(e);
}
