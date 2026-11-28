#include "RdpSession.h"
#include "core/Log.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QAxObject>
#include <QAxWidget>
#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QMimeData>
#include <QToolTip>
#include <QElapsedTimer>
#include <QEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QPushButton>
#include <QStackedLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>

#include <cmath>
#include <iterator>

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

// Only the first slot (put_ClearTextPassword) is used.
struct __declspec(uuid("c1e6743a-41c1-4a74-832a-0dd06c1c7a0e")) IMsTscNonScriptable : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE put_ClearTextPassword(BSTR pw) = 0;
};

struct __declspec(uuid("302d8188-0052-4807-806a-362b628f9ac5")) IMsRdpExtendedSettings : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE put_Property(BSTR name, VARIANT *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Property(BSTR name, VARIANT *value) = 0;
};

namespace {

// ExtendedDisconnectReasonCode values
constexpr int kExtLogoff = 2;
constexpr int kExtIdleTimeout = 3;
constexpr int kExtLogonTimeout = 4;
constexpr int kExtReplacedByOther = 5;
constexpr int kExtServerDenied = 7;
constexpr int kExtInsufficientPrivileges = 9;

// Connect() that shows no activity at all after this long failed without telling (its events do not reach Qt).
constexpr qint64 kStartTimeoutMs = 5000;
// An attempt that is still "connecting" after this long is given up.
constexpr qint64 kConnectTimeoutMs = 40000;
// refresh(): how long to wait for the old connection to report that it closed.
constexpr qint64 kRefreshWaitMs = 4000;
constexpr int kMouseSendIntervalMs = 16;
constexpr uint kNetworkWan = 5;
constexpr uint kNetworkAutoDetect = 7;
constexpr int kConnectPollMs = 150;
constexpr int kConnectedPollMs = 1000;

void ensureClipSiblings(HWND hwnd)
{
    if (!hwnd)
        return;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CLIPSIBLINGS) == 0)
        SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_CLIPSIBLINGS);
}

// Newest installed control first.
const char *const kControlClsids[] = {
    "{3F859AA3-C2D4-4FAA-B0E4-FD0C9C4E5E3A}", // v13
    "{1DF7C823-B2D4-4B54-975A-F2AC5D7CF8B8}", // v12
    "{A0C63C30-F08D-4AB4-907C-34905D770C7D}", // v11
    "{8B918B82-7985-4C24-89DF-C33AD2BBFBCD}", // v10
    "{A3BC03A0-041D-42E3-AD22-882B7865C9C5}", // v9
    "{54D38BF7-B1EF-4479-9674-1BD6EA465258}", // v8
};

uint deviceScale(uint desktopScale)
{
    return desktopScale >= 180 ? 180 : (desktopScale >= 140 ? 140 : 100);
}

} // namespace

class SessionOverlay : public QWidget {
    Q_OBJECT
public:
    enum Tone { Busy, Warning, Error };

    explicit SessionOverlay(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("sessionOverlay"));

        m_title = new QLabel(this);
        m_title->setObjectName(QStringLiteral("overlayTitle"));
        m_title->setAlignment(Qt::AlignCenter);
        m_title->setWordWrap(true);
        m_title->setFixedWidth(kTitleWidth);
        m_message = new QLabel(this);
        m_message->setObjectName(QStringLiteral("overlayMessage"));
        m_message->setAlignment(Qt::AlignCenter);
        m_message->setWordWrap(true);
        m_message->setFixedWidth(kMessageWidth);

        m_reconnect = new QPushButton(this);
        m_reconnect->setObjectName(QStringLiteral("primaryButton"));
        m_reconnect->setCursor(Qt::PointingHandCursor);
        m_close = new QPushButton(this);
        m_close->setCursor(Qt::PointingHandCursor);
        connect(m_reconnect, &QPushButton::clicked, this, &SessionOverlay::reconnectClicked);
        connect(m_close, &QPushButton::clicked, this, &SessionOverlay::closeClicked);
        connect(Theme::notifier(), &ThemeNotifier::changed, this, [this] {
            updateLabelSizes();
            update();
        });
        m_reconnect->setText(QCoreApplication::translate("RdpSession", "Reconnect"));

        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(10);
        buttons->addStretch();
        buttons->addWidget(m_reconnect);
        buttons->addWidget(m_close);
        buttons->addStretch();

        auto *lay = new QVBoxLayout(this);
        lay->setSpacing(12);
        lay->addStretch();
        lay->addSpacing(kArtHeight); // the animation is painted here
        lay->addWidget(m_title, 0, Qt::AlignHCenter);
        lay->addWidget(m_message, 0, Qt::AlignHCenter);
        lay->addSpacing(8);
        lay->addLayout(buttons);
        lay->addStretch();

        m_frame.setInterval(33);
        connect(&m_frame, &QTimer::timeout, this, [this] {
            if (window() && window()->isMinimized())
                return;
            const qint64 elapsed = m_clock.isValid() ? m_clock.elapsed() : 0;
            if (m_tone != Busy && elapsed > kAppearMs) {
                update();
                m_frame.stop();
                return;
            }
            if (elapsed > kAppearMs && m_tone == Busy) {
                // Only repaint the 120x3 progress track once the fade-in finishes.
                const QPointF c(width() / 2.0, m_title->y() - 12 - kArtHeight / 2.0);
                update(QRect(int(c.x()) - 62, int(c.y()) + 64, 124, 7));
            } else {
                update();
            }
        });
    }

    void setTexts(const QString &title, const QString &message, bool offerReconnect, const QString &closeText,
                  Tone tone = Error)
    {
        m_title->setText(title);
        m_message->setText(message);
        m_message->setVisible(!message.isEmpty());
        updateLabelSizes();
        m_reconnect->setText(QCoreApplication::translate("RdpSession", "Reconnect"));
        m_reconnect->setVisible(offerReconnect);
        m_close->setText(closeText);
        if (tone != m_tone || !m_clock.isValid()) {
            m_tone = tone;
            m_clock.restart();
        }
        if (layout())
            layout()->activate();
        if (isVisible()) {
            m_frame.start();
            update();
        }
    }

signals:
    void reconnectClicked();
    void closeClicked();

protected:
    void showEvent(QShowEvent *e) override
    {
        QWidget::showEvent(e);
        updateLabelSizes();
        m_clock.restart();
        m_frame.start();
    }

    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        updateLabelSizes();
    }

    void hideEvent(QHideEvent *e) override
    {
        QWidget::hideEvent(e);
        m_frame.stop();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), Theme::kBackground);

        const qreal ms = m_clock.isValid() ? qreal(m_clock.elapsed()) : 0.0;
        const qreal t = ms / 1000.0;
        const qreal in = qMin(1.0, ms / kAppearMs);
        const qreal ease = 1.0 - std::pow(1.0 - in, 3.0);

        // The muted colours of the theme, not bright signal colours.
        QColor tint = Theme::kLabelSecondary;
        if (m_tone == Warning)
            tint = Theme::kOrange;
        else if (m_tone == Error)
            tint = Theme::kRed;

        const QPointF c(width() / 2.0, m_title->y() - 12 - kArtHeight / 2.0);
        p.setOpacity(ease);

        // A flat tile with a monitor: no glow, gradient or pulsing.
        p.save();
        p.translate(c);
        p.setPen(Qt::NoPen);
        const QRectF tile(-30, -30, 60, 60);
        p.setBrush(Theme::kAccentSoft);
        p.drawRect(tile);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(tint, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QRectF screen(-17, -19, 34, 24);
        p.drawRect(screen);
        p.drawLine(QPointF(0, 6.5), QPointF(0, 12));
        p.drawLine(QPointF(-8, 13), QPointF(8, 13));
        p.restore();

        // Busy: a light runs along a thin bar under the tile.
        if (m_tone == Busy) {
            p.setBrush(Qt::NoBrush);
            const QRectF track(c.x() - 60, c.y() + 66, 120, 3);
            QColor trackColor = tint;
            trackColor.setAlpha(40);
            p.setPen(Qt::NoPen);
            p.setBrush(trackColor);
            p.drawRect(track);
            p.setClipRect(track);
            const qreal run = std::fmod(t / 1.4, 1.0);
            const qreal slide = run * run * (3.0 - 2.0 * run); // eased
            const qreal x = track.left() - 44 + slide * (track.width() + 88);
            p.setBrush(tint);
            p.drawRect(QRectF(x, track.top(), 44, track.height()));
            p.setClipping(false);
        }
    }

private:
    void updateLabelSizes()
    {
        const int avail = width() > 0 ? qMax(160, width() - 48) : kTitleWidth;
        const int titleW = qMin(kTitleWidth, avail);
        const int msgW = qMin(kMessageWidth, avail);

        // Word-wrapped labels report a one-line height in box layouts.
        m_title->setFixedWidth(titleW);
        m_title->ensurePolished();
        m_title->setFixedHeight(qMax(m_title->heightForWidth(titleW), m_title->sizeHint().height()));

        if (!m_message->text().isEmpty()) {
            m_message->setFixedWidth(msgW);
            m_message->ensurePolished();
            m_message->setFixedHeight(qMax(m_message->heightForWidth(msgW), m_message->sizeHint().height()));
        }
        if (layout())
            layout()->activate();
    }

    static constexpr int kTitleWidth = 560;
    static constexpr int kMessageWidth = 500;
    static constexpr int kArtHeight = 150;
    static constexpr qreal kAppearMs = 420.0;
    QLabel *m_title;
    QLabel *m_message;
    QPushButton *m_reconnect;
    QPushButton *m_close;
    QTimer m_frame;
    QElapsedTimer m_clock;
    Tone m_tone = Busy;
};

RdpSession::RdpSession(const ServerProfile &profile, QWidget *parent)
    : QWidget(parent)
    , m_profile(profile)
{
    setAcceptDrops(true);
    m_stack = new QStackedLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);

    // Native overlay above the control; covers its white surface while connecting.
    m_stack->setStackingMode(QStackedLayout::StackAll);
    m_ax = new QAxWidget(this);
    m_overlay = new SessionOverlay(this);
    m_overlay->setAttribute(Qt::WA_NativeWindow);
    m_stack->addWidget(m_ax);
    m_stack->addWidget(m_overlay);
    m_overlay->hide();

    connect(m_overlay, &SessionOverlay::reconnectClicked, this, &RdpSession::reconnect);
    connect(m_overlay, &SessionOverlay::closeClicked, this, &RdpSession::closeRequested);

    m_poll.setInterval(kConnectPollMs);
    connect(&m_poll, &QTimer::timeout, this, &RdpSession::poll);
    m_resizeTimer.setSingleShot(true);
    m_resizeTimer.setInterval(250);
    connect(&m_resizeTimer, &QTimer::timeout, this, &RdpSession::syncDisplaySize);
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, [this] {
        if (!m_closing && m_state == Connecting)
            start();
    });
}

QSize RdpSession::remoteSize() const
{
    const qreal dpr = devicePixelRatioF();
    return {qBound(200, int(width() * dpr) & ~1, 8192),
            qBound(200, int(height() * dpr) & ~1, 8192)};
}

void RdpSession::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (m_overlay->isVisible()) {
        m_overlay->setGeometry(rect());
        raiseOverlay();
    }
    if (m_state == Connected && isVisible() && !m_profile.fixedResolution().isValid())
        m_resizeTimer.start();
}

void RdpSession::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange && m_overlay->isVisible())
        showOverlay();
    // Returning to the window (Alt+Tab back, minimize/restore) leaves the focus on the frame, not the control.
    else if (e->type() == QEvent::ActivationChange && isActiveWindow())
        QTimer::singleShot(0, this, &RdpSession::focusRemote);
}

void RdpSession::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (m_overlay->isVisible()) {
        m_overlay->setGeometry(rect());
        raiseOverlay();
    }
    if (m_state == Connected && !m_profile.fixedResolution().isValid())
        m_resizeTimer.start();
    QTimer::singleShot(0, this, &RdpSession::focusRemote);
}

void RdpSession::dragEnterEvent(QDragEnterEvent *e)
{
    if (m_state == Connected && e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void RdpSession::dragMoveEvent(QDragMoveEvent *e)
{
    if (m_state == Connected && e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void RdpSession::dropEvent(QDropEvent *e)
{
    QList<QUrl> files;
    for (const QUrl &url : e->mimeData()->urls())
        if (url.isLocalFile())
            files << url;
    if (files.isEmpty())
        return;
    e->acceptProposedAction();
    sendFiles(files);
}

// The files go on this PC's clipboard, which the control shares with the server, and Ctrl+V is pressed in the
// session: Explorer on the server then pastes them into the folder that has the focus (or the desktop).
void RdpSession::sendFiles(const QList<QUrl> &files)
{
    if (!m_profile.clipboard) {
        QToolTip::showText(QCursor::pos(), tr("Sending files needs “Share clipboard” (edit the server to turn it on)."), this);
        return;
    }
    auto *mime = new QMimeData;
    mime->setUrls(files);
    QGuiApplication::clipboard()->setMimeData(mime);
    log(QStringLiteral("drop: %1 file(s) put on the clipboard").arg(files.size()));
    QToolTip::showText(QCursor::pos(), tr("Sending %1 file(s) to the server…").arg(files.size()), this);
    // Gives the control a moment to tell the server about the new clipboard.
    QTimer::singleShot(400, this, [this] {
        if (m_closing || m_state != Connected)
            return;
        focusRemote();
        INPUT keys[4] = {};
        const WORD sequence[4] = {VK_CONTROL, 'V', 'V', VK_CONTROL};
        for (int i = 0; i < 4; ++i) {
            keys[i].type = INPUT_KEYBOARD;
            keys[i].ki.wVk = sequence[i];
            keys[i].ki.wScan = WORD(MapVirtualKeyW(sequence[i], MAPVK_VK_TO_VSC));
            keys[i].ki.dwFlags = i >= 2 ? KEYEVENTF_KEYUP : 0;
        }
        SendInput(4, keys, sizeof(INPUT));
    });
}

void RdpSession::focusRemote()
{
    if (m_closing || m_state != Connected || m_ax->isNull() || !isVisible()
        || !isActiveWindow() || QApplication::activePopupWidget() || QApplication::activeModalWidget())
        return;
    m_ax->setFocus(Qt::OtherFocusReason);
    // The control installs its keyboard hook only while its own child window has the focus.
    HWND inner = static_cast<HWND>(m_innerHwnd);
    if (!inner || !IsWindow(inner)) {
        inner = nullptr;
        EnumChildWindows(reinterpret_cast<HWND>(m_ax->winId()), [](HWND h, LPARAM lp) -> BOOL {
            wchar_t cls[64];
            if (GetClassNameW(h, cls, 64) > 0 && wcscmp(cls, L"IHWindowClass") == 0) {
                *reinterpret_cast<HWND *>(lp) = h;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&inner));
        m_innerHwnd = inner;
    }
    if (inner && GetFocus() != inner)
        SetFocus(inner);
}

uint RdpSession::desktopScale() const
{
    return uint(qBound(100, qRound(devicePixelRatioF() * 100.0), 500));
}

void RdpSession::syncDisplaySize()
{
    if (m_state != Connected || m_ax->isNull() || !isVisible() || window()->isMinimized()
        || m_profile.fixedResolution().isValid())
        return;
    const QSize s = remoteSize();
    const uint scale = desktopScale();
    if (s == m_syncedSize && scale == m_syncedScale)
        return;
    m_syncedSize = s;
    m_syncedScale = scale;
    m_ax->dynamicCall("UpdateSessionDisplaySettings(uint,uint,uint,uint,uint,uint,uint)",
                      uint(s.width()), uint(s.height()), 0u, 0u, 0u, scale, deviceScale(scale));
}

void RdpSession::poll()
{
    if (m_closing || m_ax->isNull())
        return;
    const int c = m_ax->property("Connected").toInt();
    if (c != 0)
        m_sawActive = true;
    const qint64 elapsed = m_connectClock.elapsed();
    if (Log::detailed() && (c != m_lastPolled || elapsed - m_lastHeartbeat >= 5000)) {
        m_lastHeartbeat = elapsed;
        m_lastPolled = c;
        Log::detail(QStringLiteral("[%1] poll +%2ms Connected=%3 state=%4 visible=%5 ext=%6")
                .arg(m_profile.name)
                .arg(elapsed).arg(c).arg(int(m_state)).arg(isVisible())
                .arg(m_ax->property("ExtendedDisconnectReason").toInt()));
    }
    if (m_refreshing) {
        // A dead link may never report the disconnect: after a few seconds the new control is started anyway.
        if (c == 0 || elapsed - m_refreshSince > kRefreshWaitMs) {
            m_refreshing = false;
            m_poll.stop();
            m_resizeTimer.stop();
            setState(Idle);
            start();
        }
        return;
    }
    if (c == 1 && m_state != Connected) {
        onLoginComplete();
    } else if (c == 0 && m_sawActive && (m_state == Connecting || m_state == Connected)) {
        handleDisconnected(0);
    } else if (m_state == Connecting && m_connectCalledAt >= 0) {
        // The control's events do not reach Qt, so a Connect() that failed at once (no activity at all) or one that
        // hangs would otherwise leave the tab on "Reaching the server…" for good.
        const qint64 since = elapsed - m_connectCalledAt;
        if (!m_sawActive && since > kStartTimeoutMs) {
            log(QStringLiteral("Connect() did not start a connection"));
            attemptFailed(0, true);
        } else if (c == 2 && since > kConnectTimeoutMs) {
            log(QStringLiteral("connecting took longer than %1 s; giving up this attempt").arg(kConnectTimeoutMs / 1000));
            m_ax->dynamicCall("Disconnect()");
            attemptFailed(264, m_retryAttempt > 0);
        }
    }
}

bool RdpSession::scheduleRetry()
{
    static const int delays[] = {1500, 4000, 8000};
    if (m_retryAttempt >= int(std::size(delays)))
        return false;
    const int delay = delays[m_retryAttempt];
    ++m_retryAttempt;
    log(QStringLiteral("automatic reconnect %1 in %2 ms").arg(m_retryAttempt).arg(delay));
    setState(Connecting);
    showOverlay();
    m_retryTimer.start(delay);
    return true;
}

void RdpSession::attemptFailed(int reason, bool retry)
{
    m_poll.stop();
    m_resizeTimer.stop();
    m_lastReason = reason;
    m_lastExtReason = 0;
    if (retry && scheduleRetry())
        return;
    setState(m_everConnected ? Disconnected : Failed);
    showOverlay();
}

void RdpSession::repaintRemote()
{
    if (m_ax->isNull())
        return;
    RedrawWindow(reinterpret_cast<HWND>(m_ax->winId()), nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN);
}

bool RdpSession::createControl()
{
    if (!m_ax->isNull())
        return true;
    static QString s_cachedClsid;
    if (!s_cachedClsid.isEmpty()) {
        if (!m_ax->setControl(s_cachedClsid))
            s_cachedClsid.clear();
    }
    if (m_ax->isNull()) {
        for (const char *clsid : kControlClsids) {
            const QString id = QString::fromLatin1(clsid);
            const bool ok = m_ax->setControl(id);
            log(QStringLiteral("setControl %1 -> %2").arg(id, ok ? "ok" : "failed"));
            if (ok) {
                s_cachedClsid = id;
                break;
            }
        }
    }
    if (m_ax->isNull()) {
        log(QStringLiteral("no RDP ActiveX control could be created"));
        return false;
    }

    connect(m_ax, SIGNAL(OnConnecting()), this, SLOT(onConnecting()));
    connect(m_ax, SIGNAL(OnConnected()), this, SLOT(onConnected()));
    connect(m_ax, SIGNAL(OnLoginComplete()), this, SLOT(onLoginComplete()));
    connect(m_ax, SIGNAL(OnDisconnected(int)), this, SLOT(onDisconnected(int)));
    // Only connect the catch-all COM signal handler when detailed logging is enabled.
    if (Log::detailed()) {
        connect(m_ax, &QAxWidget::signal, this, [this](const QString &name, int argc, void *) {
            if (Log::detailed())
                Log::detail(QStringLiteral("[%1] event %2 (%3 args)").arg(m_profile.name, name).arg(argc));
        });
    }
    connect(m_ax, &QAxWidget::exception, this, [this](int code, const QString &src, const QString &desc, const QString &help) {
        log(QStringLiteral("COM exception %1 src=%2 desc=%3 help=%4").arg(code).arg(src, desc, help));
    });
    return true;
}

void RdpSession::replaceControl()
{
    m_innerHwnd = nullptr;
    QAxWidget *old = m_ax;
    old->disconnect(this);
    old->hide();
    if (old->property("Connected").toInt() != 0)
        old->dynamicCall("Disconnect()");
    m_stack->removeWidget(old);
    old->deleteLater();

    m_ax = new QAxWidget(this);
    m_stack->insertWidget(0, m_ax);
    m_ax->show();
    m_stack->activate();
    if (m_overlay->isVisible())
        raiseOverlay();
    else
        m_overlay->raise();
    m_syncedSize = {};
    m_syncedScale = 0;
    log(QStringLiteral("control replaced"));
}

bool RdpSession::worthRetrying(bool wasConnected) const
{
    constexpr int kMaxRetries = 3;
    if (m_retryAttempt >= kMaxRetries || (!wasConnected && m_retryAttempt == 0))
        return false;
    // Someone ended the session on purpose, or retrying cannot help.
    switch (m_lastExtReason) {
    case kExtLogoff:
    case kExtIdleTimeout:
    case kExtLogonTimeout:
    case kExtReplacedByOther:
    case kExtServerDenied:
    case kExtInsufficientPrivileges:
        return false;
    default:
        break;
    }
    if (m_lastReason == 0 && m_lastExtReason == 0)
        return false; // closed by the server, perhaps because another computer connected
    return m_lastReason != 1 && m_lastReason != 2 && m_lastReason != 3 && m_lastReason != 260 && m_lastReason != 1798;
}

void RdpSession::applySettings()
{
    const QSize fixed = m_profile.fixedResolution();
    const QSize size = fixed.isValid() ? fixed : remoteSize();

    log(QStringLiteral("applySettings host=%1 port=%2 user=%3 domain=%4 size=%5x%6 fixedRes=%7 passwordSet=%8 ignoreCert=%9")
            .arg(m_profile.host).arg(m_profile.port).arg(m_profile.user, m_profile.domain)
            .arg(size.width()).arg(size.height()).arg(fixed.isValid())
            .arg(!m_profile.password.isEmpty()).arg(m_profile.ignoreCertWarnings));
    m_ax->setProperty("Server", m_profile.host);
    m_ax->setProperty("UserName", m_profile.user);
    m_ax->setProperty("Domain", m_profile.domain);
    m_ax->setProperty("ColorDepth", m_profile.colorDepth);
    m_ax->setProperty("DesktopWidth", size.width());
    m_ax->setProperty("DesktopHeight", size.height());

    QAxObject *adv = m_ax->querySubObject("AdvancedSettings9");
    if (!adv)
        adv = m_ax->querySubObject("AdvancedSettings8");
    log(QStringLiteral("AdvancedSettings object: %1").arg(adv ? "found" : "MISSING"));
    if (adv) {
        adv->setProperty("RDPPort", m_profile.port);
        adv->setProperty("SmartSizing", fixed.isValid());
        adv->setProperty("RedirectClipboard", m_profile.clipboard);
        adv->setProperty("RedirectDrives", m_profile.drives);
        adv->setProperty("RedirectPrinters", false);
        adv->setProperty("RedirectSmartCards", false);
        adv->setProperty("RedirectPorts", false);
        adv->setProperty("AudioRedirectionMode", m_profile.audio ? 0 : 2); // 0 local, 2 off
        adv->setProperty("AudioCaptureRedirectionMode", false);
        adv->setProperty("EnableCredSspSupport", true);
        adv->setProperty("AuthenticationLevel", m_profile.ignoreCertWarnings ? 0 : 2);
        // Off: the control's own retry shows a Windows dialog and holds the control for a long time.
        // RdpSession reconnects with a fresh control instead (see handleDisconnected).
        adv->setProperty("EnableAutoReconnect", false);
        // A link that died without a word (sleep, Wi-Fi, VPN) is noticed in seconds instead of minutes, so the tab
        // reconnects instead of showing a frozen picture.
        adv->setProperty("keepAliveInterval", 10000);
        adv->setProperty("BitmapPeristence", 1);
        adv->setProperty("Compress", 1);
        // Keep visual effects light when the server cannot determine the connection type.
        adv->setProperty("PerformanceFlags", 0x1 | 0x2 | 0x4 | 0x20 | 0x80);
        if (!adv->setProperty("NetworkConnectionType", kNetworkAutoDetect))
            adv->setProperty("NetworkConnectionType", kNetworkWan);
        const bool bandwidthDetectionSet = adv->setProperty("BandwidthDetection", true);
        const int mouseInterval = adv->property("minInputSendInterval").toInt();
        if (mouseInterval > kMouseSendIntervalMs)
            adv->setProperty("minInputSendInterval", kMouseSendIntervalMs);
        log(QStringLiteral("RDP network type=%1 bandwidth detection requested=%2 mouse interval=%3 ms")
                .arg(adv->property("NetworkConnectionType").toUInt())
                .arg(bandwidthDetectionSet)
                .arg(adv->property("minInputSendInterval").toInt()));
    }

    // Windows key, Alt+Tab and the like go to the server while this tab has the keyboard, not to this PC. The control
    // defaults to 2 (only in full screen); the property is on SecuredSettings2, not on AdvancedSettings.
    if (QAxObject *secured = m_ax->querySubObject("SecuredSettings2")) {
        secured->setProperty("KeyboardHookMode", 1);
        log(QStringLiteral("KeyboardHookMode = %1").arg(secured->property("KeyboardHookMode").toString()));
    } else {
        log(QStringLiteral("SecuredSettings2 object: MISSING (Windows key and Alt+Tab stay on this PC)"));
    }

    const uint scale = fixed.isValid() ? 0 : desktopScale();
    IMsRdpExtendedSettings *ext = nullptr;
    if (m_ax->queryInterface(QUuid(QStringLiteral("{302d8188-0052-4807-806a-362b628f9ac5}")),
                                reinterpret_cast<void **>(&ext)) == S_OK && ext) {
        const auto put = [ext](const wchar_t *name, VARIANT value) {
            BSTR n = SysAllocString(name);
            const HRESULT result = ext->put_Property(n, &value);
            SysFreeString(n);
            return result;
        };
        VARIANT value{};
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_TRUE;
        const HRESULT hardware = put(L"EnableHardwareMode", value);
        const HRESULT urcp = put(L"UseURCP", value); // Used only when the RDP connection negotiates UDP.
        Log::detail(QStringLiteral("[%1] RDP hardware decoding hr=0x%2 UDP rate control hr=0x%3")
                        .arg(m_profile.name).arg(quint32(hardware), 8, 16, QLatin1Char('0'))
                        .arg(quint32(urcp), 8, 16, QLatin1Char('0')));
        if (!fixed.isValid()) {
            value.vt = VT_UI4;
            value.ulVal = scale;
            put(L"DesktopScaleFactor", value);
            value.ulVal = deviceScale(scale);
            put(L"DeviceScaleFactor", value);
        }
        ext->Release();
    }
    m_syncedSize = size;
    m_syncedScale = scale;

    if (!m_profile.password.isEmpty()) {
        IMsTscNonScriptable *ns = nullptr;
        if (m_ax->queryInterface(QUuid(QStringLiteral("{c1e6743a-41c1-4a74-832a-0dd06c1c7a0e}")),
                                 reinterpret_cast<void **>(&ns)) == S_OK && ns) {
            BSTR pw = SysAllocString(reinterpret_cast<const OLECHAR *>(m_profile.password.utf16()));
            const HRESULT hr = ns->put_ClearTextPassword(pw);
            SecureZeroMemory(pw, SysStringByteLen(pw));
            SysFreeString(pw);
            ns->Release();
            log(QStringLiteral("put_ClearTextPassword hr=0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0')));
        } else {
            log(QStringLiteral("IMsTscNonScriptable not available; password NOT set"));
        }
    } else {
        log(QStringLiteral("no saved password; the control may wait for credentials"));
    }
}

void RdpSession::start()
{
    m_closing = false;
    m_everConnected = false;
    m_checkingServer = false;
    m_serverViewSet = false;
    m_noteTitle.clear();
    m_noteMessage.clear();
    m_lastReason = m_lastExtReason = 0;
    m_lastPolled = -1;
    m_lastHeartbeat = 0;
    m_connectCalledAt = -1;
    m_refreshing = false;
    m_retryTimer.stop();
    m_connectClock.start();
    log(QStringLiteral("start (log: %1)").arg(Log::path()));
    if (!m_ax->isNull())
        replaceControl();
    if (!createControl()) {
        m_controlMissing = true;
        setState(Failed);
        showOverlay();
        return;
    }
    if (m_ax->isHidden())
        m_ax->show();
    setState(Connecting);
    showOverlay();
    m_sawActive = false;
    // Wait for the final tab size.
    QTimer::singleShot(0, this, [this] {
        if (m_closing) {
            log(QStringLiteral("start aborted: session closing"));
            return;
        }
        applySettings();
        log(QStringLiteral("calling Connect()"));
        m_ax->dynamicCall("Connect()");
        log(QStringLiteral("Connect() returned"));
        if (m_overlay->isVisible())
            raiseOverlay();
        m_connectCalledAt = m_connectClock.elapsed();
        m_poll.start(m_state == Connected ? kConnectedPollMs : kConnectPollMs);
    });
}

void RdpSession::setServerView(const QString &title, const QString &message, bool replaced)
{
    m_checkingServer = false;
    m_serverViewSet = true;
    m_noteTitle = title;
    m_noteMessage = message;
    if (replaced && m_state == Disconnected)
        setState(Replaced);
    if (m_state == Replaced || m_state == Disconnected || m_state == Failed)
        showOverlay();
}

void RdpSession::setCheckingServer(bool checking)
{
    if (m_checkingServer == checking)
        return;
    m_checkingServer = checking;
    if (m_state == Replaced || m_state == Disconnected || m_state == Failed)
        showOverlay();
}

void RdpSession::reconnect()
{
    if (m_state == Connecting || m_state == Connected)
        return;
    m_retryAttempt = 0;
    start();
}

void RdpSession::refresh()
{
    if (m_state != Connected && m_state != Connecting) {
        start();
        return;
    }
    // The server session stays logged in.
    m_refreshing = true;
    m_refreshSince = m_connectClock.elapsed();
    m_poll.setInterval(kConnectPollMs);
    m_ax->dynamicCall("Disconnect()");
}

void RdpSession::shutdown()
{
    m_closing = true;
    m_poll.stop();
    m_retryTimer.stop();
    hide();
    if (m_ax->isNull() || m_state == Idle || m_state == Disconnected || m_state == Replaced || m_state == Failed) {
        deleteLater();
        return;
    }
    m_ax->dynamicCall("Disconnect()");
    // In case OnDisconnected never arrives.
    QTimer::singleShot(1500, this, &QObject::deleteLater);
}

void RdpSession::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(s);
}

void RdpSession::log(const QString &text) const
{
    Log::write(QStringLiteral("[%1] %2").arg(m_profile.name, text));
}

void RdpSession::onConnecting()
{
    log(QStringLiteral("OnConnecting"));
    setState(Connecting);
    if (m_overlay->isVisible())
        raiseOverlay();
}

void RdpSession::onConnected()
{
    log(QStringLiteral("OnConnected (transport up)"));
    m_everConnected = true;
    if (m_state == Connecting && m_overlay->isVisible())
        showOverlay();
}

void RdpSession::onLoginComplete()
{
    if (m_closing || m_refreshing || m_state == Connected)
        return;

    log(QStringLiteral("login complete"));
    m_everConnected = true;
    m_retryAttempt = 0;
    if (m_ax->isHidden())
        m_ax->show();
    m_stack->setCurrentWidget(m_ax);
    m_overlay->hide();
    m_poll.setTimerType(Qt::CoarseTimer);
    m_poll.setInterval(kConnectedPollMs);
    setState(Connected);
    repaintRemote();
    focusRemote();
    const QSize before = m_syncedSize;
    const uint beforeScale = m_syncedScale;
    syncDisplaySize();
    // The display channel may not be open yet at login; the first resize can be lost.
    if (m_syncedSize != before || m_syncedScale != beforeScale) {
        QTimer::singleShot(2000, m_ax, [this, control = m_ax] {
            if (control == m_ax && m_state == Connected && !m_refreshing) {
                m_syncedScale = 0;
                syncDisplaySize();
            }
        });
    }
}

QString RdpSession::connectedUser() const
{
    if (m_state != Connected || m_ax->isNull())
        return {};
    const QString user = m_ax->property("UserName").toString();
    if (user.isEmpty() || user.contains(QLatin1Char('\\')) || user.contains(QLatin1Char('@')))
        return user;
    const QString domain = m_ax->property("Domain").toString();
    return domain.isEmpty() ? user : domain + QLatin1Char('\\') + user;
}

void RdpSession::onDisconnected(int reason)
{
    log(QStringLiteral("OnDisconnected reason=%1 closing=%2 refreshing=%3").arg(reason).arg(m_closing).arg(m_refreshing));
    if (m_closing) {
        deleteLater();
        return;
    }
    if (m_refreshing)
        return;
    handleDisconnected(reason);
}

void RdpSession::handleDisconnected(int reason)
{
    if (m_state != Connecting && m_state != Connected)
        return; // already handled
    m_poll.stop();
    m_resizeTimer.stop();

    m_lastReason = reason;
    m_lastExtReason = m_ax->property("ExtendedDisconnectReason").toInt();
    log(QStringLiteral("disconnected reason=%1 extReason=%2 everConnected=%3")
            .arg(m_lastReason).arg(m_lastExtReason).arg(m_everConnected));
    const bool wasConnected = m_state == Connected;
    // A dropped link: look again shortly with a fresh control, without a dialog.
    if (worthRetrying(wasConnected) && scheduleRetry())
        return;
    if (m_lastExtReason == kExtReplacedByOther)
        setState(Replaced);
    else
        setState(m_everConnected ? Disconnected : Failed);
    showOverlay();
}

void RdpSession::raiseOverlay()
{
    if (!m_overlay->isVisible())
        return;
    m_overlay->raise();
    HWND overlayHwnd = reinterpret_cast<HWND>(m_overlay->winId());
    if (!overlayHwnd)
        return;
    ensureClipSiblings(overlayHwnd);
    SetWindowPos(overlayHwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (m_ax && m_ax->isVisible()) {
        if (HWND axHwnd = reinterpret_cast<HWND>(m_ax->winId())) {
            ensureClipSiblings(axHwnd);
            SetWindowPos(axHwnd, overlayHwnd, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

void RdpSession::showOverlay()
{
    // A disconnected or failed ActiveX control is never reused (start() replaces it). Hiding it
    // unmaps its Win32 child windows so mstscax's white surface can never cover the overlay.
    const bool keepControl = !m_controlMissing && (m_state == Connecting || m_state == Connected);
    if (!keepControl) {
        m_innerHwnd = nullptr;
        if (m_ax->isVisible())
            m_ax->hide();
    }

    const QString closeTab = tr("Close Tab");
    if (m_controlMissing) {
        m_overlay->setTexts(tr("RDP component not found"),
                            tr("The Microsoft Remote Desktop client control (mstscax.dll) could not be loaded on this PC."),
                            false, closeTab);
    } else if (m_state == Connecting || m_state == Idle) {
        // The server answered (transport is up): now it signs in.
        m_overlay->setTexts(tr("Connecting to “%1”…").arg(m_profile.name),
                            m_retryAttempt > 0 ? tr("The connection was lost. Trying again (%1/3)…").arg(m_retryAttempt)
                            : m_everConnected  ? tr("Signing in…")
                                               : tr("Reaching the server…"),
                            false, tr("Cancel"), SessionOverlay::Busy);
    } else if (m_state == Replaced) {
        // The server said another connection took over; once it has been asked who is connected, that answer wins.
        QString msg;
        if (!m_noteTitle.isEmpty()) {
            msg = m_noteMessage;
        } else if (m_checkingServer) {
            msg = tr("Another computer has connected to “%1”.\n\nChecking who connected…").arg(m_profile.name);
        } else {
            msg = tr("Another computer has connected to “%1”. Reconnecting may disconnect the other user.")
                      .arg(m_profile.name)
                  + (m_noteMessage.isEmpty() ? QString() : QStringLiteral("\n\n") + m_noteMessage);
        }
        m_overlay->setTexts(m_noteTitle.isEmpty() ? tr("Another computer connected") : m_noteTitle,
                            msg, true, closeTab, SessionOverlay::Warning);
    } else {
        const QString text = describe(m_lastReason, m_lastExtReason);
        const QString extra = m_checkingServer ? tr("Checking who is connected…") : m_noteMessage;
        QString msg;
        if (!m_noteTitle.isEmpty())
            msg = m_noteMessage;
        else if (extra.isEmpty())
            msg = text;
        else if (text.isEmpty())
            msg = extra;
        else
            msg = text + QStringLiteral("\n\n") + extra;
        m_overlay->setTexts(m_noteTitle.isEmpty() ? (m_everConnected ? tr("Connection ended") : tr("Could not connect"))
                                                  : m_noteTitle,
                            msg, true, closeTab, SessionOverlay::Error);
    }
    m_stack->setCurrentWidget(m_overlay);
    if (m_overlay->geometry() != rect())
        m_overlay->setGeometry(rect());
    m_overlay->show();
    raiseOverlay();
    m_overlay->update();
    if (!keepControl && isVisible() && isActiveWindow() && !QApplication::activePopupWidget()
        && !QApplication::activeModalWidget()) {
        m_overlay->setFocus(Qt::OtherFocusReason);
        if (HWND overlayHwnd = reinterpret_cast<HWND>(m_overlay->winId()); overlayHwnd && GetFocus() != overlayHwnd)
            SetFocus(overlayHwnd);
    }
}

QString RdpSession::describe(int reason, int ext) const
{
    switch (ext) {
    case kExtLogoff:
        return tr("The remote session was logged off (by the user or an administrator).");
    case kExtIdleTimeout:
        return tr("The server ended the session because it was idle for too long.");
    case kExtLogonTimeout:
        return tr("The sign-in timed out.");
    case kExtServerDenied:
        return tr("The server refused the connection. Check the user name, password and remote desktop permission.");
    case kExtInsufficientPrivileges:
        return tr("This user is not allowed to connect to the server remotely.");
    default:
        break;
    }
    if (reason == 0 && ext == 0) {
        // No reason given: do not guess who did it. Workspace asks the server and fills in who is connected.
        return m_everConnected ? tr("The server closed the connection without saying why.") : QString();
    }
    switch (reason) {
    case 260:  return tr("The server name could not be resolved (DNS). Is the address correct?");
    case 264:  return tr("The connection timed out. The server may be off or a firewall is blocking it.");
    case 516:  return tr("The server could not be reached. Is Remote Desktop enabled on it?");
    case 1798: return tr("The server certificate could not be verified.");
    case 2308: return tr("The network connection was lost.");
    default: break;
    }
    QString text = m_ax->dynamicCall("GetErrorDescription(uint,uint)", uint(reason), uint(ext)).toString();
    if (text.isEmpty())
        text = tr("The connection was lost.");
    return tr("%1 (code %2/%3)").arg(text).arg(reason).arg(ext);
}

#include "RdpSession.moc"
