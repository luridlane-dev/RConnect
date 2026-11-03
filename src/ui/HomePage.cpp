#include "HomePage.h"

#include "AccessLogPage.h"
#include "LiveSessionsPage.h"
#include "SessionWindow.h"
#include "ServerPanel.h"
#include "Theme.h"
#include "UsagePage.h"
#include "core/Autostart.h"
#include "core/Language.h"
#include "core/UpdateChecker.h"
#include "model/ServerStore.h"

#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QEvent>
#include <QFrame>
#include <QPainter>
#include <QPropertyAnimation>
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

// Grey, and white once the button is checked.
QIcon navIcon(Glyph glyph)
{
    QIcon icon;
    icon.addPixmap(glyphPixmap(glyph, Theme::kLabelSecondary), QIcon::Normal, QIcon::Off);
    icon.addPixmap(glyphPixmap(glyph, Theme::kLabel), QIcon::Normal, QIcon::On);
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
    m_pillAnim->setDuration(280);
    m_pillAnim->setEasingCurve(QEasingCurve::OutCubic);
    sidebar->installEventFilter(this);

    auto *logo = new QLabel(sidebar);
    logo->setPixmap(QIcon(QStringLiteral(":/icon.png")).pixmap(30, 30));
    auto *name = new QLabel(QStringLiteral("RConnect"), sidebar);
    name->setObjectName(QStringLiteral("brandName"));
    m_brandSubtitle = new QLabel(sidebar);
    m_brandSubtitle->setObjectName(QStringLiteral("brandSummary"));
    auto *brandText = new QVBoxLayout;
    brandText->setSpacing(0);
    brandText->addWidget(name);
    brandText->addWidget(m_brandSubtitle);
    auto *brand = new QHBoxLayout;
    brand->setContentsMargins(8, 0, 0, 0);
    brand->setSpacing(10);
    brand->addWidget(logo);
    brand->addLayout(brandText, 1);
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

    m_operatorCaption = new QLabel(sidebar);
    m_operatorCaption->setObjectName(QStringLiteral("brandSummary"));
    auto *operatorName = new QLabel(qEnvironmentVariable("USERDOMAIN") + QLatin1Char('\\') +qEnvironmentVariable("USERNAME"),
                                    sidebar);
    operatorName->setObjectName(QStringLiteral("operatorName"));
    operatorName->setToolTip(operatorName->text());
    // Lined up with the text of the buttons (their padding)
    m_operatorCaption->setContentsMargins(13, 0, 0, 0);
    operatorName->setContentsMargins(13, 0, 0, 0);
    side->addWidget(m_operatorCaption);
    side->addWidget(operatorName);
    side->addSpacing(6);

    m_settings = new QPushButton(sidebar);
    m_settings->setObjectName(QStringLiteral("navButton"));
    m_settings->setCursor(Qt::PointingHandCursor);
    m_settings->setIcon(navIcon(Gear));
    m_settings->setIconSize(QSize(18, 18));
    connect(m_settings, &QPushButton::clicked, this, &HomePage::showSettingsMenu);
    side->addWidget(m_settings);

    // Pages
    m_pages = new QStackedWidget(this);

    auto *serversPage = new QWidget(m_pages);
    auto *serversLay = new QVBoxLayout(serversPage);
    serversLay->setContentsMargins(0, 0, 0, 0);
    serversLay->setSpacing(14);
    m_title = new QLabel(serversPage);
    m_title->setObjectName(QStringLiteral("pageTitle"));
    m_summary = new QLabel(serversPage);
    m_summary->setObjectName(QStringLiteral("pageSubtitle"));
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(m_title);
    titles->addWidget(m_summary);
    auto *head = new QHBoxLayout;
    head->setContentsMargins(ServerPanel::kInset, 0, ServerPanel::kInset, 0);
    head->addLayout(titles);
    head->addStretch(1);
    serversLay->addLayout(head);
    m_brandLayout = head;

    // Counts at a glance.
    m_tiles = new QHBoxLayout;
    m_tiles->setContentsMargins(ServerPanel::kInset, 0, ServerPanel::kInset, 0);
    m_tiles->setSpacing(10);
    const char *const tones[] = {"", "green", "orange", "red"};
    for (int i = 0; i < 4; ++i) {
        auto *tile = new QFrame(serversPage);
        tile->setObjectName(QStringLiteral("statTile"));
        auto *tileLay = new QVBoxLayout(tile);
        tileLay->setContentsMargins(16, 12, 16, 12);
        tileLay->setSpacing(0);
        m_tileValue[i] = new QLabel(tile);
        m_tileValue[i]->setObjectName(QStringLiteral("statValue"));
        m_tileValue[i]->setProperty("tone", QLatin1String(tones[i]));
        m_tileCaption[i] = new QLabel(tile);
        m_tileCaption[i]->setObjectName(QStringLiteral("statCaption"));
        tileLay->addWidget(m_tileValue[i]);
        tileLay->addWidget(m_tileCaption[i]);
        m_tiles->addWidget(tile, 1);
    }
    serversLay->addLayout(m_tiles);
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

    connect(m_nav, &QButtonGroup::idClicked, this, [this](int i) {
        if (i == m_pages->currentIndex())
            return;
        m_pages->setCurrentIndex(i);
        movePill(true);
    });
    connect(m_panel, &ServerPanel::rightInsetChanged, this, [this](int inset) {
        m_brandLayout->setContentsMargins(ServerPanel::kInset, 0, inset, 0);
        m_tiles->setContentsMargins(ServerPanel::kInset, 0, inset, 0);
    });
    connect(store, &ServerStore::summaryChanged, this, &HomePage::updateSummary);
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

void HomePage::retranslate()
{
    m_brandSubtitle->setText(tr("Remote desktop manager"));
    m_operatorCaption->setText(tr("Windows user"));
    m_settings->setText(tr("Settings"));
    const QStringList names = {tr("Servers"), tr("Who's connected"), tr("Time on servers"), tr("Activity history")};
    for (int i = 0; i < names.size(); ++i)
        m_nav->button(i)->setText(names.at(i));
    updateSummary();
}

void HomePage::updateSummary()
{
    m_title->setText(tr("Servers"));
    m_summary->setText(ServerPanel::summary(m_store));
    m_summary->setVisible(false); // the tiles below say it
    const ServerStore::Counts c = m_store->counts();
    const int values[] = {c.total, c.online - c.busy, c.busy, c.offline};
    const QString captions[] = {tr("Servers"), tr("Available"), tr("In use"), tr("Offline")};
    for (int i = 0; i < 4; ++i) {
        m_tileValue[i]->setText(QString::number(values[i]));
        m_tileCaption[i]->setText(captions[i]);
    }
}

void HomePage::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange)
        retranslate();
}

void HomePage::showSettingsMenu()
{
    QMenu menu(this);
    QMenu *language = menu.addMenu(tr("Language"));
    auto *group = new QActionGroup(language);
    for (const QString &code : Language::available()) {
        QAction *a = language->addAction(Language::nativeName(code));
        a->setCheckable(true);
        a->setChecked(code == Language::current());
        group->addAction(a);
        connect(a, &QAction::triggered, this, [code] { Language::set(code); });
    }
    QMenu *appearance = menu.addMenu(tr("Appearance"));
    auto *appearanceGroup = new QActionGroup(appearance);
    for (Theme::Mode m : {Theme::Dark, Theme::Light}) {
        QAction *a = appearance->addAction(m == Theme::Dark ? tr("Dark") : tr("Light"));
        a->setCheckable(true);
        a->setChecked(Theme::mode() == m);
        appearanceGroup->addAction(a);
        connect(a, &QAction::triggered, this, [m] { Theme::setMode(m); });
    }
    menu.addSeparator();
    QMenu *buttons = menu.addMenu(tr("Window buttons in full screen"));
    auto *buttonGroup = new QActionGroup(buttons);
    for (bool mac : {false, true}) {
        QAction *a = buttons->addAction(mac ? tr("macOS style") : tr("Windows style"));
        a->setCheckable(true);
        a->setChecked(SessionWindow::macButtons() == mac);
        buttonGroup->addAction(a);
        connect(a, &QAction::triggered, this, [mac] { SessionWindow::setMacButtons(mac); });
    }
    menu.addSeparator();
    QAction *autostart = menu.addAction(tr("Start with Windows"));
    autostart->setCheckable(true);
    autostart->setChecked(Autostart::isEnabled());
    connect(autostart, &QAction::toggled, this, [](bool on) { Autostart::setEnabled(on); });
    menu.addSeparator();
    menu.addAction(tr("Check for updates…"), this, &HomePage::updateCheckRequested);
    QAction *autoUpdate = menu.addAction(tr("Check for updates automatically"));
    autoUpdate->setCheckable(true);
    autoUpdate->setChecked(UpdateChecker::autoCheckEnabled());
    connect(autoUpdate, &QAction::toggled, this, [](bool on) { UpdateChecker::setAutoCheckEnabled(on); });
    menu.addSeparator();
    menu.addAction(tr("About RConnect"), this, &HomePage::showAbout);
    menu.exec(m_settings->mapToGlobal(QPoint(0, 0)) - QPoint(0, menu.sizeHint().height() + 4));
}

void HomePage::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle(tr("About RConnect"));
    box.setIconPixmap(QIcon(QStringLiteral(":/icon.png")).pixmap(56, 56));
    box.setText(QStringLiteral("<b>RConnect %1</b>").arg(QApplication::applicationVersion()));
    box.setInformativeText(tr("Saves your Remote Desktop servers and connects to them in one click.")
                           + QStringLiteral("<br><br>MIT License<br>"
                                            "<a href=\"https://db-ip.com\" style=\"color:#5cb0ee\">IP Geolocation by DB-IP</a>"));
    box.setTextFormat(Qt::RichText);
    box.setTextInteractionFlags(Qt::TextBrowserInteraction);
    box.exec();
}
