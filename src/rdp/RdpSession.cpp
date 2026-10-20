#include "RdpSession.h"

#include <QAxObject>
#include <QAxWidget>
#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QStackedLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>

#include <objbase.h>
#include <oleauto.h>

// Only the first slot (put_ClearTextPassword) is used.
struct __declspec(uuid("c1e6743a-41c1-4a74-832a-0dd06c1c7a0e")) IMsTscNonScriptable : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE put_ClearTextPassword(BSTR pw) = 0;
};

namespace {

// ExtendedDisconnectReasonCode values
constexpr int kExtLogoff = 2;
constexpr int kExtIdleTimeout = 3;
constexpr int kExtLogonTimeout = 4;
constexpr int kExtReplacedByOther = 5;
constexpr int kExtServerDenied = 7;
constexpr int kExtInsufficientPrivileges = 9;

// Newest installed control first.
const char *const kControlClsids[] = {
    "{3F859AA3-C2D4-4FAA-B0E4-FD0C9C4E5E3A}", // v13
    "{1DF7C823-B2D4-4B54-975A-F2AC5D7CF8B8}", // v12
    "{A0C63C30-F08D-4AB4-907C-34905D770C7D}", // v11
    "{8B918B82-7985-4C24-89DF-C33AD2BBFBCD}", // v10
    "{A3BC03A0-041D-42E3-AD22-882B7865C9C5}", // v9
    "{54D38BF7-B1EF-4479-9674-1BD6EA465258}", // v8
};

} // namespace

class SessionOverlay : public QWidget {
    Q_OBJECT
public:
    explicit SessionOverlay(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("sessionOverlay"));
        setAttribute(Qt::WA_StyledBackground, true);

        m_title = new QLabel(this);
        m_title->setObjectName(QStringLiteral("overlayTitle"));
        m_title->setAlignment(Qt::AlignCenter);
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
        lay->addWidget(m_title);
        lay->addWidget(m_message, 0, Qt::AlignHCenter);
        lay->addSpacing(8);
        lay->addLayout(buttons);
        lay->addStretch();
    }

    void setTexts(const QString &title, const QString &message, bool offerReconnect, const QString &closeText)
    {
        m_title->setText(title);
        m_message->setText(message);
        m_message->setVisible(!message.isEmpty());
        // Word-wrapped labels report a one-line height in box layouts.
        m_message->setFixedHeight(m_message->heightForWidth(kMessageWidth));
        m_reconnect->setText(QCoreApplication::translate("RdpSession", "Reconnect"));
        m_reconnect->setVisible(offerReconnect);
        m_close->setText(closeText);
    }

signals:
    void reconnectClicked();
    void closeClicked();

private:
    static constexpr int kMessageWidth = 460;
    QLabel *m_title;
    QLabel *m_message;
    QPushButton *m_reconnect;
    QPushButton *m_close;
};

RdpSession::RdpSession(const ServerProfile &profile, QWidget *parent)
    : QWidget(parent)
    , m_profile(profile)
{
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

    m_poll.setInterval(500);
    connect(&m_poll, &QTimer::timeout, this, &RdpSession::poll);
    m_resizeTimer.setSingleShot(true);
    m_resizeTimer.setInterval(250);
    connect(&m_resizeTimer, &QTimer::timeout, this, &RdpSession::syncDisplaySize);
}

QSize RdpSession::remoteSize() const
{
    const qreal dpr = devicePixelRatioF();
    // Even, 200..8192
    const int w = qBound(200, int(m_ax->width() * dpr) & ~1, 8192);
    const int h = qBound(200, int(m_ax->height() * dpr) & ~1, 8192);
    return {w, h};
}

void RdpSession::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (m_state == Connected && !m_profile.fixedResolution().isValid())
        m_resizeTimer.start();
}

void RdpSession::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange && m_overlay->isVisible())
        showOverlay();
}

void RdpSession::syncDisplaySize()
{
    if (m_state != Connected || m_ax->isNull() || m_profile.fixedResolution().isValid())
        return;
    const QSize s = remoteSize();
    const uint scale = uint(qMax(100.0, devicePixelRatioF() * 100.0));
    const uint deviceScale = scale >= 180 ? 180 : (scale >= 140 ? 140 : 100);
    m_ax->dynamicCall("UpdateSessionDisplaySettings(uint,uint,uint,uint,uint,uint,uint)",
                      uint(s.width()), uint(s.height()), 0u, 0u, 0u, scale, deviceScale);
}

void RdpSession::poll()
{
    if (m_closing || m_ax->isNull())
        return;
    const int c = m_ax->property("Connected").toInt();
    if (c != 0)
        m_sawActive = true;
    if (m_refreshing) {
        if (c == 0) {
            m_refreshing = false;
            m_poll.stop();
            m_resizeTimer.stop();
            setState(Idle);
            start();
        }
        return;
    }
    if (c == 1 && m_state != Connected)
        onLoginComplete();
    else if (c == 0 && m_sawActive && (m_state == Connecting || m_state == Connected))
        handleDisconnected(0);
}

bool RdpSession::createControl()
{
    if (!m_ax->isNull())
        return true;
    for (const char *clsid : kControlClsids) {
        if (m_ax->setControl(QString::fromLatin1(clsid)))
            break;
    }
    if (m_ax->isNull())
        return false;

    connect(m_ax, SIGNAL(OnConnecting()), this, SLOT(onConnecting()));
    connect(m_ax, SIGNAL(OnConnected()), this, SLOT(onConnected()));
    connect(m_ax, SIGNAL(OnLoginComplete()), this, SLOT(onLoginComplete()));
    connect(m_ax, SIGNAL(OnDisconnected(int)), this, SLOT(onDisconnected(int)));
    return true;
}

void RdpSession::applySettings()
{
    const QSize fixed = m_profile.fixedResolution();
    const QSize size = fixed.isValid() ? fixed : remoteSize();

    m_ax->setProperty("Server", m_profile.host);
    m_ax->setProperty("UserName", m_profile.user);
    m_ax->setProperty("Domain", m_profile.domain);
    m_ax->setProperty("ColorDepth", m_profile.colorDepth);
    m_ax->setProperty("DesktopWidth", size.width());
    m_ax->setProperty("DesktopHeight", size.height());

    QAxObject *adv = m_ax->querySubObject("AdvancedSettings9");
    if (!adv)
        adv = m_ax->querySubObject("AdvancedSettings8");
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
        adv->setProperty("EnableAutoReconnect", true);
        adv->setProperty("MaxReconnectAttempts", 5);
        adv->setProperty("BitmapPeristence", 1);
        adv->setProperty("Compress", 1);
        // No wallpaper, full-window drag, menu animations or cursor shadow.
        adv->setProperty("PerformanceFlags", 0x1 | 0x2 | 0x4 | 0x20 | 0x80);
        adv->setProperty("NetworkConnectionType", 7); // auto-detect
        adv->setProperty("BandwidthDetection", true);
    }

    if (!m_profile.password.isEmpty()) {
        IMsTscNonScriptable *ns = nullptr;
        if (m_ax->queryInterface(QUuid(QStringLiteral("{c1e6743a-41c1-4a74-832a-0dd06c1c7a0e}")),
                                 reinterpret_cast<void **>(&ns)) == S_OK && ns) {
            BSTR pw = SysAllocString(reinterpret_cast<const OLECHAR *>(m_profile.password.utf16()));
            ns->put_ClearTextPassword(pw);
            SecureZeroMemory(pw, SysStringByteLen(pw));
            SysFreeString(pw);
            ns->Release();
        }
    }
}

void RdpSession::start()
{
    m_closing = false;
    m_everConnected = false;
    m_lastReason = m_lastExtReason = 0;
    if (!createControl()) {
        m_controlMissing = true;
        setState(Failed);
        showOverlay();
        return;
    }
    setState(Connecting);
    showOverlay();
    m_sawActive = false;
    // Wait for the final tab size.
    QTimer::singleShot(0, this, [this] {
        if (m_closing)
            return;
        applySettings();
        m_ax->dynamicCall("Connect()");
        m_poll.start();
    });
}

void RdpSession::reconnect()
{
    if (m_state == Connecting || m_state == Connected)
        return;
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
    m_ax->dynamicCall("Disconnect()");
}

void RdpSession::shutdown()
{
    m_closing = true;
    m_poll.stop();
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

void RdpSession::onConnecting()
{
    setState(Connecting);
}

void RdpSession::onConnected()
{
    m_everConnected = true;
}

void RdpSession::onLoginComplete()
{
    m_everConnected = true;
    m_overlay->hide();
    m_ax->setFocus();
    setState(Connected);
    syncDisplaySize();
}

void RdpSession::onDisconnected(int reason)
{
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
    if (m_lastExtReason == kExtReplacedByOther)
        setState(Replaced);
    else
        setState(m_everConnected ? Disconnected : Failed);
    showOverlay();
}

void RdpSession::showOverlay()
{
    const QString closeTab = tr("Close Tab");
    if (m_controlMissing) {
        m_overlay->setTexts(tr("RDP component not found"),
                            tr("The Microsoft Remote Desktop client control (mstscax.dll) could not be loaded on this PC."),
                            false, closeTab);
    } else if (m_state == Connecting || m_state == Idle) {
        m_overlay->setTexts(tr("Connecting to “%1”…").arg(m_profile.name), QString(), false, tr("Cancel"));
    } else if (m_state == Replaced) {
        m_overlay->setTexts(tr("Another computer connected"),
                            tr("Another computer has connected to “%1”. Reconnecting may disconnect the other user.")
                                .arg(m_profile.name),
                            true, closeTab);
    } else {
        m_overlay->setTexts(m_everConnected ? tr("Connection ended") : tr("Could not connect"),
                            describe(m_lastReason, m_lastExtReason), true, closeTab);
    }
    m_overlay->show();
    m_overlay->raise();
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
        return m_everConnected ? tr("The server closed the connection. Another computer may have connected.")
                               : QString();
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
