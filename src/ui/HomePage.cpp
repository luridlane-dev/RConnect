#include "HomePage.h"

#include "AccessLogPage.h"
#include "ContactsDialog.h"
#include "LiveSessionsPage.h"
#include "SessionWindow.h"
#include "SettingsDialog.h"
#include "ServerPanel.h"
#include "Theme.h"
#include "UsagePage.h"
#include "core/AccessLog.h"
#include "core/Autostart.h"
#include "core/Language.h"
#include "core/Log.h"
#include "core/UpdateChecker.h"
#include "model/ServerStore.h"

#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QDesktopServices>
#include <QFileInfo>
#include <QUrl>
#include <QEvent>
#include <QFrame>
#include <QPainter>
#include <QPropertyAnimation>
#include <QSettings>
#include <QShowEvent>
#include <QTimer>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

enum Glyph { Monitor, People, Bars, Clock, Gear };

// Line icon in one colour, drawn at 2x so it stays sharp.
QPixmap glyphPixmap(Glyph glyph, const QColor &color)
{
    constexpr qreal dpr = 2.0;
    QPixmap pm(int(18 * dpr), int(18 * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    switch (glyph) {
    case Monitor:
        p.drawRoundedRect(QRectF(2, 3, 14, 9.5), 2, 2);
        p.drawLine(QPointF(9, 12.5), QPointF(9, 15));
        p.drawLine(QPointF(5.5, 15.2), QPointF(12.5, 15.2));
        break;
    case People:
        p.drawEllipse(QPointF(7, 6.5), 2.8, 2.8);
        p.drawArc(QRectF(2, 10.5, 10, 8), 0, 180 * 16);
        p.drawEllipse(QPointF(13, 7.2), 2.1, 2.1);
        p.drawArc(QRectF(11, 11.5, 6, 6), 20 * 16, 140 * 16);
        break;
    case Bars:
        p.drawLine(QPointF(4, 14), QPointF(4, 9));
        p.drawLine(QPointF(9, 14), QPointF(9, 4));
        p.drawLine(QPointF(14, 14), QPointF(14, 7));
        break;
    case Clock:
        p.drawEllipse(QPointF(9, 9), 6.8, 6.8);
        p.drawLine(QPointF(9, 5.2), QPointF(9, 9));
        p.drawLine(QPointF(9, 9), QPointF(11.8, 10.8));
        break;
    case Gear:
        p.drawEllipse(QPointF(9, 9), 2.4, 2.4);
        for (int k = 0; k < 8; ++k) {
            p.save();
            p.translate(9, 9);
            p.rotate(k * 45);
            p.drawLine(QPointF(0, -5.4), QPointF(0, -7.4));
            p.restore();
        }
        p.drawEllipse(QPointF(9, 9), 5.4, 5.4);
        break;
    }
    return pm;
}

constexpr Glyph kNavGlyphs[4] = {Monitor, People, Bars, Clock};

// The three monitoring pages only get a button in the navigation when they are turned on in the settings; they
// are off by default. Turning one off hides the button only: what is recorded does not change.
struct OptionalPage {
    const char *key;
};
constexpr OptionalPage kOptionalPages[4] = {{nullptr}, {"pages/live"}, {"pages/usage"}, {"pages/log"}};

bool pageEnabled(int index)
{
    return !kOptionalPages[index].key || QSettings().value(QLatin1String(kOptionalPages[index].key), false).toBool();
}

// Grey, and white once the button is checked.
QIcon navIcon(Glyph glyph)
{
    QIcon icon;
    icon.addPixmap(glyphPixmap(glyph, Theme::kLabelSecondary), QIcon::Normal, QIcon::Off);
    icon.addPixmap(glyphPixmap(glyph, Qt::white), QIcon::Normal, QIcon::On);
    return icon;
}

} // namespace

HomePage::HomePage(ServerStore *store, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
{
    setObjectName(QStringLiteral("emptyPage"));

    // Navigation
    auto *sidebar = new QFrame(this);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(236);
    auto *side = new QVBoxLayout(sidebar);
    side->setContentsMargins(14, 22, 14, 16);
    side->setSpacing(4);

    // Created before the buttons, so it lies behind them.
    m_navPill = new QFrame(sidebar);
    m_navPill->setObjectName(QStringLiteral("navPill"));
    m_navPill->hide();
    m_pillAnim = new QPropertyAnimation(m_navPill, "geometry", this);
    m_pillAnim->setDuration(140);
    m_pillAnim->setEasingCurve(QEasingCurve::OutCubic);
    sidebar->installEventFilter(this);

    auto *logo = new QLabel(sidebar);
    logo->setPixmap(QIcon(QStringLiteral(":/icon.png")).pixmap(30, 30));
    auto *name = new QLabel(QStringLiteral("RConnect"), sidebar);
    name->setObjectName(QStringLiteral("brandName"));
    auto *brand = new QHBoxLayout;
    brand->setContentsMargins(8, 0, 0, 0);
    brand->setSpacing(10);
    brand->addWidget(logo);
    brand->addWidget(name, 1);
    side->addLayout(brand);
    side->addSpacing(22);

    m_nav = new QButtonGroup(this);
    m_nav->setExclusive(true);
    for (int i = 0; i < 4; ++i) {
        auto *b = new QPushButton(sidebar);
        b->setObjectName(QStringLiteral("navButton"));
        b->setCheckable(true);
        b->setChecked(i == 0);
        b->setCursor(Qt::PointingHandCursor);
        b->setIcon(navIcon(kNavGlyphs[i]));
        b->setIconSize(QSize(18, 18));
        m_nav->addButton(b, i);
        side->addWidget(b);
    }
    side->addStretch(1);

    m_settings = new QPushButton(sidebar);
    m_settings->setObjectName(QStringLiteral("navButton"));
    m_settings->setCursor(Qt::PointingHandCursor);
    m_settings->setIcon(navIcon(Gear));
    m_settings->setIconSize(QSize(18, 18));
    connect(m_settings, &QPushButton::clicked, this, &HomePage::showSettings);
    side->addWidget(m_settings);

    // Pages
    m_pages = new QStackedWidget(this);

    auto *serversPage = new QWidget(m_pages);
    auto *serversLay = new QVBoxLayout(serversPage);
    serversLay->setContentsMargins(0, 0, 0, 0);
    serversLay->setSpacing(10);
    m_title = new QLabel(serversPage);
    m_title->setObjectName(QStringLiteral("pageTitle"));
    // The counts are on the filter buttons below.
    auto *head = new QHBoxLayout;
    head->setContentsMargins(ServerPanel::kInset, 0, ServerPanel::kInset, 0);
    head->setSpacing(12);
    head->addWidget(m_title, 0, Qt::AlignBottom);
    head->addStretch(1);
    serversLay->addLayout(head);
    m_brandLayout = head;

    m_panel = new ServerPanel(store, serversPage);
    serversLay->addWidget(m_panel, 1);
    m_pages->addWidget(serversPage);

    m_live = new LiveSessionsPage(store, m_pages);
    m_pages->addWidget(m_live);
    m_usage = new UsagePage(m_pages);
    m_pages->addWidget(m_usage);
    m_log = new AccessLogPage(m_pages);
    m_pages->addWidget(m_log);

    auto *content = new QWidget(this);
    auto *contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(28, 26, 28, 20);
    contentLay->addWidget(m_pages);

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(sidebar);
    lay->addWidget(content, 1);

    applyPages();
    connect(m_nav, &QButtonGroup::idClicked, this, [this](int i) {
        if (i == m_pages->currentIndex())
            return;
        m_pages->setCurrentIndex(i);
        movePill(true);
    });
    connect(m_panel, &ServerPanel::rightInsetChanged, this, [this](int inset) {
        m_brandLayout->setContentsMargins(ServerPanel::kInset, 0, inset, 0);
    });
    connect(Theme::notifier(), &ThemeNotifier::changed, this, [this] {
        for (int i = 0; i < 4; ++i)
            m_nav->button(i)->setIcon(navIcon(kNavGlyphs[i]));
        m_settings->setIcon(navIcon(Gear));
    });
    retranslate();
}

void HomePage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (m_shown)
        return;
    m_shown = true;
    QTimer::singleShot(0, this, [this] { movePill(false); });
}

bool HomePage::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::Resize && m_navPill && watched == m_navPill->parentWidget())
        QTimer::singleShot(0, this, [this] { movePill(false); });
    return QWidget::eventFilter(watched, e);
}

// The highlight slides to the button of the current page.
void HomePage::movePill(bool animate)
{
    QAbstractButton *b = m_nav->checkedButton();
    if (!b)
        return;
    const QRect target = b->geometry();
    m_pillAnim->stop();
    if (!animate || !m_navPill->isVisible()) {
        m_navPill->setGeometry(target);
        m_navPill->show();
        return;
    }
    m_pillAnim->setStartValue(m_navPill->geometry());
    m_pillAnim->setEndValue(target);
    m_pillAnim->start();
}

// Shows the buttons of the pages that are turned on. If the page being viewed was turned off, the server list comes back.
void HomePage::applyPages()
{
    for (int i = 1; i < 4; ++i)
        m_nav->button(i)->setVisible(pageEnabled(i));
    if (!pageEnabled(m_pages->currentIndex())) {
        m_pages->setCurrentIndex(0);
        m_nav->button(0)->setChecked(true);
    }
    QTimer::singleShot(0, this, [this] { movePill(false); });
}

void HomePage::retranslate()
{
    m_settings->setText(tr("Settings"));
    const QStringList names = {tr("Servers"), tr("Who's connected"), tr("Time on servers"), tr("Activity history")};
    for (int i = 0; i < names.size(); ++i)
        m_nav->button(i)->setText(names.at(i));
    updateSummary();
}

void HomePage::updateSummary()
{
    m_title->setText(tr("Servers"));
}

void HomePage::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange)
        retranslate();
}

void HomePage::showSettings()
{
    SettingsDialog dialog(m_store, window());
    connect(&dialog, &SettingsDialog::pagesChanged, this, &HomePage::applyPages);
    connect(&dialog, &SettingsDialog::importRequested, this, [this] { QTimer::singleShot(0, this, &HomePage::importRequested); });
    connect(&dialog, &SettingsDialog::exportRequested, this, [this] { QTimer::singleShot(0, this, &HomePage::exportRequested); });
    connect(&dialog, &SettingsDialog::updateCheckRequested, this, [this] { QTimer::singleShot(0, this, &HomePage::updateCheckRequested); });
    connect(&dialog, &SettingsDialog::aboutRequested, this, &HomePage::showAbout);
    dialog.exec();
}

void HomePage::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle(tr("About RConnect"));
    box.setIconPixmap(QIcon(QStringLiteral(":/icon.png")).pixmap(56, 56));
    box.setText(QStringLiteral("<b>RConnect %1</b>").arg(QApplication::applicationVersion()));
    box.setInformativeText(tr("Saves your Remote Desktop servers and connects to them in one click.")
                           + QStringLiteral("<br><br>")
                           + tr("Developed by Lurid Lane")
                           + QStringLiteral("<br><a href=\"https://github.com/luridlane-dev/RConnect\" style=\"color:#7da9dc\">"
                                            "github.com/luridlane-dev/RConnect</a><br><br>MIT License<br>"
                                            "<a href=\"https://db-ip.com\" style=\"color:#7da9dc\">IP Geolocation by DB-IP</a>"));
    box.setTextFormat(Qt::RichText);
    box.setTextInteractionFlags(Qt::TextBrowserInteraction);
    box.exec();
}
