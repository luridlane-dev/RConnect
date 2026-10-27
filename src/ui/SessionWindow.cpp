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
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QScreen>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QIcon dotIcon(const QColor &c)
{
    QPixmap pm(32, 32);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QRectF(4, 5, 8, 8));
    return QIcon(pm);
}

QColor sessionColor(RdpSession::State s)
{
    switch (s) {
    case RdpSession::Connected:    return QColor(0x30, 0xd1, 0x58);
    case RdpSession::Connecting:   return QColor(0x0a, 0x84, 0xff);
    case RdpSession::Replaced:     return QColor(0xff, 0x9f, 0x0a);
    case RdpSession::Failed:
    case RdpSession::Disconnected: return QColor(0xff, 0x45, 0x3a);
    default:                       return QColor(0x8e, 0x8e, 0x93);
    }
}

} // namespace

SessionWindow::SessionWindow(QWidget *home, QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_home(home)
{
    setObjectName(QStringLiteral("root"));
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

    m_tabBar = new AnimatedTabBar(m_topBar);
    connect(m_tabBar, &QTabBar::currentChanged, this, &SessionWindow::onTabChanged);
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

    // Shown only in full screen.
    const char *const glyphs[] = {"–", "□", "×"};
    for (int i = 0; i < 3; ++i) {
        auto *b = new QToolButton(m_topBar);
        b->setObjectName(i == 2 ? QStringLiteral("winClose") : QStringLiteral("winButton"));
        b->setText(QString::fromUtf8(glyphs[i]));
        b->setFixedSize(40, AnimatedTabBar::kHeight);
        b->setCursor(Qt::PointingHandCursor);
        b->hide();
        bar->addWidget(b);
        m_winButtons << b;
    }
    connect(m_winButtons[0], &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(m_winButtons[1], &QToolButton::clicked, this, &SessionWindow::toggleFullscreen);
    connect(m_winButtons[2], &QToolButton::clicked, this, &QWidget::close);

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
    Theme::darkTitleBar(this); // also creates the native window
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
    const QString id = s->serverId();
    m_sessions.insert(id, s);
    m_sessionStack->addWidget(s);

    const int idx = m_tabBar->insertTab(index < 0 ? m_tabBar->count() : index,
                                        dotIcon(sessionColor(s->state())), s->serverName());
    m_tabBar->setTabData(idx, id);
    auto *close = new QToolButton(m_tabBar);
    close->setObjectName(QStringLiteral("tabClose"));
    close->setText(QStringLiteral("×"));
    close->setToolTip(tr("Close tab (Ctrl+W)"));
    close->setCursor(Qt::PointingHandCursor);
    close->setFixedSize(20, 20);
    connect(close, &QToolButton::clicked, this, [this, id] { emit closeTabRequested(id); });
    m_tabBar->setTabButton(idx, QTabBar::RightSide, close);
    m_tabBar->setCurrentIndex(idx);
    onTabChanged(idx);
    updatePage();
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
    const bool mark = attention && i != m_tabBar->currentIndex();
    m_tabBar->setTabText(i, mark ? s->serverName() + QStringLiteral("  !") : s->serverName());
    if (i == m_tabBar->currentIndex())
        updateTitle();
}

void SessionWindow::onTabChanged(int index)
{
    if (index < 0)
        return;
    if (RdpSession *s = m_sessions.value(m_tabBar->tabData(index).toString())) {
        m_sessionStack->setCurrentWidget(s);
        m_tabBar->setTabText(index, s->serverName()); // clears the "!" marker
        s->setFocus();
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
    if (m_home)
        m_pages->setCurrentIndex(m_tabBar->count() > 0 ? 1 : 0);
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
    m_winButtons[0]->setToolTip(tr("Minimise"));
    m_winButtons[1]->setToolTip(tr("Exit full screen (F11)"));
    m_winButtons[2]->setToolTip(tr("Close"));
    for (int i = 0; i < m_tabBar->count(); ++i)
        if (QWidget *b = m_tabBar->tabButton(i, QTabBar::RightSide))
            b->setToolTip(tr("Close tab (Ctrl+W)"));
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
        // Maximize means full screen here; a maximized window would leave the taskbar visible.
        if ((windowState() & Qt::WindowMaximized) && !(windowState() & Qt::WindowFullScreen))
            QTimer::singleShot(0, this, [this] {
                if (isMaximized() && !isFullScreen())
                    showFullScreen();
            });
        for (QToolButton *b : std::as_const(m_winButtons))
            b->setVisible(isFullScreen());
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
