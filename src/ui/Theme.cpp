#include "Theme.h"

#include <QApplication>
#include <QFont>
#include <QHash>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
#include <QWidget>

#include <windows.h>
#include <dwmapi.h>

namespace {

// Tokens in @name@ are replaced by the colours of the current mode.
const char *const kStyleSheet = R"(
QWidget { color: @text@; font-family: "Segoe UI"; font-size: 13px; }
QWidget#root { background: @bg@; }
QMainWindow, QDialog, QMessageBox { background: @bg@; }

QWidget#topBar { background: @topBar@; border-bottom: 1px solid @border@; }
QToolButton#menuButton {
    background: transparent; border: none; border-radius: 2px; color: @text2@; padding: 0 0 3px 0; margin: 3px 2px; font-size: 16px;
}
QToolButton#menuButton:hover { background: @hover@; color: @text@; }
QToolButton#menuButton:pressed { background: @press@; }
QToolButton#homeTab {
    background: transparent; border: none; border-radius: 2px; padding: 0; margin: 3px 2px 3px 4px;
    min-width: 28px; max-width: 28px;
}
QToolButton#homeTab:hover { background: @hover@; }
QToolButton#homeTab:checked { background: @tabHome@; }
QToolButton#winButton {
    background: transparent; border: none; border-radius: 0; color: @text2@; padding: 0; font-size: 13px;
}
QToolButton#winButton:hover { background: @hover@; color: @text@; }
QToolButton#winClose {
    background: transparent; border: none; border-radius: 0; color: @text2@; padding: 0; font-size: 15px;
}
QToolButton#winClose:hover { background: #c42b1c; color: #ffffff; }
QToolButton#trafficClose, QToolButton#trafficMin, QToolButton#trafficMax {
    border: none; border-radius: 2px; padding: 0; min-width: 12px; max-width: 12px; min-height: 12px; max-height: 12px;
}
QToolButton#trafficClose { background: #ff5f57; }
QToolButton#trafficMin { background: #febc2e; }
QToolButton#trafficMax { background: #28c840; }
QToolButton#trafficClose:hover { background: #ff8780; }
QToolButton#trafficMin:hover { background: #ffd060; }
QToolButton#trafficMax:hover { background: #5bdc70; }
QFrame#popup { background: @solid@; border: 1px solid @solidBorder@; border-radius: 2px; }
QLabel#brandName { font-size: 16px; font-weight: 700; color: @text@; }
QLabel#brandSummary { color: @text2@; font-size: 12px; }
QLabel#pageTitle { font-size: 20px; font-weight: 700; color: @text@; }
QLabel#pageSubtitle { color: @text2@; font-size: 13px; }
QLabel#operatorName { color: @text@; font-size: 12px; font-weight: 600; }
QFrame#sidebar { background: @sidebar@; border-right: 1px solid @border@; }
QFrame#detailCard { background: @surface@; border: 1px solid @border@; border-radius: 2px; }
QFrame#navPill { background: @accent@; border-radius: 2px; }
QPushButton#navButton {
    background: transparent; border: none; border-radius: 2px; color: @text2@; text-align: left;
    padding: 6px 12px; font-size: 13px; font-weight: 500;
}
QPushButton#navButton:hover { background: @hoverSoft@; color: @text@; }
QPushButton#navButton:checked { background: transparent; color: #ffffff; font-weight: 600; }
QLabel#emptyState { color: @text3@; font-size: 14px; }
QLabel#popupTitle { font-size: 13px; font-weight: 600; color: @text@; }
QLabel#fieldHint { color: @text2@; font-size: 12px; }
QLabel#settingsHeading { color: @text2@; font-size: 11px; font-weight: 600; }
QToolButton#barButton {
    background: transparent; border: none; border-radius: 2px; color: @text2@; padding: 0 12px; font-size: 13px;
}
QToolButton#barButton:hover { background: @hover@; color: @text@; }
QToolButton#settingsButton {
    font-family: "Segoe Fluent Icons", "Segoe MDL2 Assets"; font-size: 16px; background: @button@;
    border: none; border-radius: 2px; color: @text2@; padding: 0;
}
QToolButton#settingsButton:hover { background: @hover@; color: @text@; }
QToolButton#sectionToggle {
    background: transparent; border: none; color: @accentText@; padding: 2px 0; font-weight: 600;
}
QToolButton#sectionToggle:hover { color: @accent@; }
QPlainTextEdit {
    background: @field@; border: 1px solid @border@; border-radius: 2px; padding: 5px 8px;
    selection-background-color: @accent@; selection-color: #ffffff;
}
QPlainTextEdit:focus { border-color: @accent@; }

QListView { background: transparent; border: none; outline: none; }
QTableView {
    background: @surface@; alternate-background-color: @surfaceAlt@; border: 1px solid @border@; border-radius: 2px;
    gridline-color: transparent; outline: none; selection-background-color: @accent@; selection-color: #ffffff;
}
QTableView::item { padding: 3px 8px; border: none; }
QHeaderView::section {
    background: @surfaceAlt@; color: @text2@; border: none; border-bottom: 1px solid @border@; padding: 6px 8px;
    font-weight: 600;
}
QTableView QScrollBar:horizontal { height: 8px; margin: 2px; background: transparent; }
QTableView QScrollBar::handle:horizontal { background: @scroll@; border-radius: 2px; min-width: 36px; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: @scroll@; border-radius: 2px; min-height: 36px; }
QScrollBar::handle:vertical:hover { background: @scrollHover@; }
QScrollBar::handle:vertical:disabled { background: transparent; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QPushButton, QToolButton {
    background: @segOn@; border: 1px solid @border@; border-radius: 2px; padding: 5px 12px;
}
QPushButton:hover, QToolButton:hover { background: @hover@; }
QPushButton:pressed, QToolButton:pressed { background: @press@; }
QPushButton:disabled { background: @surface@; color: @text3@; }
QPushButton#primaryButton { background: @accent@; border-color: transparent; color: #ffffff; font-weight: 600; }
QPushButton#primaryButton:disabled { background: @surface@; color: @text3@; }
QPushButton#primaryButton:hover { background: @accentHover@; }
QPushButton#primaryButton:pressed { background: @accentPress@; }
QPushButton#addButton {
    background: @accent@; border-color: transparent; color: #ffffff; font-weight: 600; border-radius: 2px;
    padding: 0 18px; min-height: 34px;
}
QPushButton#addButton:hover { background: @accentHover@; }
QPushButton#addButton:pressed { background: @accentPress@; }

QFrame#segmented { background: @segTrack@; border-radius: 2px; }
QPushButton#segment {
    background: transparent; color: @text2@; border: none; border-radius: 2px; padding: 0 14px;
    min-height: 26px; font-weight: 500;
}
QPushButton#segment:hover { color: @text@; background: transparent; }
QPushButton#segment:checked { background: @accent@; color: #ffffff; border: none; font-weight: 600; }

QLineEdit, QSpinBox, QComboBox {
    background: @field@; border: 1px solid @border@; border-radius: 2px; padding: 5px 9px;
    selection-background-color: @accent@; selection-color: #ffffff;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border: 1px solid @accent@; }
QLineEdit#searchField { border-radius: 2px; padding: 0 14px; min-height: 32px; }
QComboBox QAbstractItemView {
    background: @solid@; border: 1px solid @solidBorder@; border-radius: 2px; selection-background-color: @accent@;
    selection-color: #ffffff; outline: none;
}
QCheckBox { spacing: 10px; }
QCheckBox::indicator { width: 16px; height: 16px; border-radius: 1px; border: 1.5px solid @text3@; background: transparent; }
QCheckBox::indicator:checked { background: @accent@; border-color: @accent@; }

QTabBar { background: transparent; }
QTabBar::tab {
    background: transparent; color: @text2@; padding: 0 4px 0 20px; margin: 0; border: none; font-size: 12px;
    min-width: 80px; max-width: 180px;
}
QTabBar::tab:hover { color: @text@; background: @hoverSoft@; }
QTabBar::tab:selected { color: #ffffff; background: transparent; font-weight: 600; }
QToolButton#tabClose {
    background: transparent; border: none; border-radius: 2px; color: @text2@; font-size: 13px; padding: 0; margin: 0;
}
QToolButton#tabClose:hover { background: @hover@; color: @text@; }
QToolButton#tabClose:pressed { background: @press@; }

QWidget#emptyPage { background: transparent; }
QScrollArea#settingsScroll, QWidget#settingsContent { background: transparent; border: none; }

QWidget#sessionOverlay { background: @overlay@; }
QLabel#overlayTitle { font-size: 22px; font-weight: 700; color: @overlayText@; }
QLabel#overlayMessage { color: @overlayText2@; font-size: 14px; }

QMenu { background: @solid@; border: 1px solid @solidBorder@; border-radius: 2px; padding: 6px; }
QMenu::item { padding: 6px 20px; border-radius: 2px; }
QMenu::item:selected { background: @accent@; color: #ffffff; }
QMenu::separator { height: 1px; background: @solidBorder@; margin: 4px 8px; }
QToolTip { background: @solid@; color: @text@; border: 1px solid @solidBorder@; border-radius: 2px; padding: 5px 9px; font-size: 12px; }
)";

// Every colour of one mode. The style sheet tokens and the colours used by hand-painted widgets both come from
// here, so the two cannot drift apart. Surfaces are opaque (cheaper to paint, and the same on any background);
// only hover, press and scroll handles are translucent because they sit on whatever is below them.
struct Scheme {
    QColor bg, topBar, surface, surfaceAlt, sidebar, button, field, border, solid, solidBorder;
    QColor text, text2, text3;
    QColor hover, hoverSoft, press, tabPill, tabActive, tabHome, segTrack, segOn, scroll, scrollHover;
    QColor accent, accentHover, accentPress, accentText;
    QColor green, orange, red, busy, busyText;
    QColor overlay, overlayText, overlayText2;
    QColor cardHover, cardPressed, separator, separatorStrong, barOff, iconLine, iconTile;
    QColor headFill;
};

Scheme darkScheme()
{
    Scheme c;
    c.bg = QColor(0x1b, 0x1e, 0x23);
    c.topBar = QColor(0x14, 0x16, 0x1a);
    c.surface = QColor(0x24, 0x28, 0x2e);
    c.surfaceAlt = QColor(0x20, 0x23, 0x29);
    c.sidebar = QColor(0x17, 0x19, 0x1e);
    c.button = QColor(0x2c, 0x31, 0x38);
    c.field = QColor(0x19, 0x1b, 0x20);
    c.border = QColor(0x36, 0x3b, 0x43);
    c.solid = QColor(0x27, 0x2b, 0x31);
    c.solidBorder = QColor(0x3c, 0x42, 0x4a);
    c.text = QColor(0xe4, 0xe7, 0xeb);
    c.text2 = QColor(0xa3, 0xaa, 0xb4);
    c.text3 = QColor(0x78, 0x80, 0x8b);
    c.hover = QColor(255, 255, 255, 20);
    c.hoverSoft = QColor(255, 255, 255, 12);
    c.press = QColor(255, 255, 255, 32);
    c.tabPill = QColor(255, 255, 255, 30);
    // The tab in front: a muted blue with white text, so it cannot be mistaken for the others on the dark bar.
    c.tabActive = QColor(0x33, 0x51, 0x76);
    c.tabHome = QColor(0x2d, 0x32, 0x39);
    c.segTrack = QColor(0x19, 0x1b, 0x20);
    c.segOn = QColor(0x31, 0x37, 0x3f);
    c.scroll = QColor(255, 255, 255, 46);
    c.scrollHover = QColor(255, 255, 255, 90);
    c.accent = QColor(0x44, 0x79, 0xb4);
    c.accentHover = QColor(0x51, 0x88, 0xc4);
    c.accentPress = QColor(0x39, 0x6a, 0x9f);
    c.accentText = QColor(0x86, 0xad, 0xdc);
    c.green = QColor(0x70, 0xa1, 0x86);
    c.orange = QColor(0xc4, 0xa0, 0x66);
    c.red = QColor(0xc4, 0x7a, 0x72);
    c.busy = QColor(0xd2, 0x6a, 0x60);
    c.busyText = QColor(0xf0, 0x8c, 0x82);
    c.overlay = c.bg;
    c.overlayText = c.text;
    c.overlayText2 = c.text2;
    c.cardHover = QColor(0x2b, 0x30, 0x37);
    c.cardPressed = QColor(0x20, 0x23, 0x29);
    c.separator = QColor(255, 255, 255, 18);
    c.separatorStrong = QColor(255, 255, 255, 34);
    c.barOff = QColor(255, 255, 255, 40);
    c.iconLine = QColor(0xd1, 0xd5, 0xdb);
    c.iconTile = QColor(0x4f, 0x5d, 0x6b);
    c.headFill = QColor(0x2e, 0x33, 0x3b);
    return c;
}

Scheme lightScheme()
{
    Scheme c;
    c.bg = QColor(0xf3, 0xf4, 0xf6);
    c.topBar = QColor(0xe3, 0xe6, 0xea);
    c.surfaceAlt = QColor(0xf7, 0xf8, 0xf9);
    c.sidebar = QColor(0xec, 0xee, 0xf1);
    c.border = QColor(0xd0, 0xd4, 0xda);
    c.solidBorder = QColor(0xc6, 0xcb, 0xd2);
    c.text = QColor(0x1f, 0x23, 0x28);
    c.text2 = QColor(0x4d, 0x55, 0x60);
    c.text3 = QColor(0x73, 0x7b, 0x86);
    c.segTrack = QColor(0xe4, 0xe7, 0xeb);
    c.accent = QColor(0x2f, 0x65, 0xa2);
    c.accentHover = QColor(0x3a, 0x73, 0xb3);
    c.accentPress = QColor(0x27, 0x55, 0x8a);
    c.green = QColor(0x55, 0x8c, 0x6c);
    c.orange = QColor(0xa9, 0x80, 0x46);
    c.red = QColor(0xb0, 0x66, 0x5e);
    c.busy = QColor(0xc2, 0x48, 0x3c);
    c.busyText = QColor(0xb0, 0x28, 0x1e);
    c.cardHover = QColor(0xee, 0xf3, 0xf9);
    c.separator = QColor(0, 0, 0, 22);
    c.separatorStrong = QColor(0, 0, 0, 46);
    c.surface = QColor(0xff, 0xff, 0xff);
    c.button = QColor(0xff, 0xff, 0xff);
    c.field = c.surface;
    c.solid = QColor(0xff, 0xff, 0xff);
    c.hover = QColor(0, 0, 0, 15);
    c.hoverSoft = QColor(0, 0, 0, 10);
    c.press = QColor(0, 0, 0, 26);
    c.tabPill = QColor(0, 0, 0, 34);
    c.tabActive = QColor(0x3a, 0x67, 0x9a);
    c.tabHome = QColor(0xff, 0xff, 0xff);
    c.segOn = QColor(0xff, 0xff, 0xff);
    c.scroll = QColor(0, 0, 0, 56);
    c.scrollHover = QColor(0, 0, 0, 107);
    c.accentText = c.accent;
    c.overlay = c.bg;
    c.overlayText = c.text;
    c.overlayText2 = c.text2;
    c.cardPressed = QColor(0xec, 0xee, 0xf1);
    c.barOff = QColor(0, 0, 0, 36);
    c.iconLine = QColor(0x3a, 0x3a, 0x3f);
    c.iconTile = QColor(0x5b, 0x6b, 0x7c);
    c.headFill = QColor(0xe9, 0xed, 0xf2);
    return c;
}

QString css(const QColor &c)
{
    if (c.alpha() == 255)
        return c.name();
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
}

Theme::Mode g_mode = Theme::Dark;
bool g_loaded = false;

void readMode()
{
    if (g_loaded)
        return;
    g_loaded = true;
    g_mode = QSettings().value(QStringLiteral("theme")).toString() == QLatin1String("light") ? Theme::Light : Theme::Dark;
}

void setColours(const Scheme &c)
{
    using namespace Theme;
    kBackground = c.bg;
    kCard = c.surface;
    kCardHover = c.cardHover;
    kCardPressed = c.cardPressed;
    kSeparator = c.separator;
    kSeparatorStrong = c.separatorStrong;
    kLabel = c.text;
    kLabelSecondary = c.text2;
    kLabelTertiary = c.text3;
    kBlue = c.accent;
    kBlueText = c.accentText;
    kGreen = c.green;
    kOrange = c.orange;
    kRed = c.red;
    kBusy = c.busy;
    kBusyText = c.busyText;
    kTabPill = c.tabPill;
    kTabActive = c.tabActive;
    kBarOff = c.barOff;
    kPopupFill = c.solid;
    kPopupBorder = c.solidBorder;
    kIconLine = c.iconLine;
    kIconTile = c.iconTile;
    kHeaderFill = c.headFill;
}

void applyMode(QApplication &app)
{
    const bool dark = g_mode == Theme::Dark;
    const Scheme c = dark ? darkScheme() : lightScheme();
    setColours(c);

    QPalette p;
    p.setColor(QPalette::Window, c.bg);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.field);
    p.setColor(QPalette::AlternateBase, c.surfaceAlt);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.button);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::ToolTipBase, c.solid);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::Highlight, c.accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, c.text2);
    p.setColor(QPalette::Disabled, QPalette::Text, c.text3);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.text3);
    app.setPalette(p);

    const QHash<QString, QColor> tokens = {
        {"bg", c.bg}, {"topBar", c.topBar}, {"text", c.text}, {"text2", c.text2}, {"text3", c.text3},
        {"surface", c.surface}, {"surfaceAlt", c.surfaceAlt}, {"sidebar", c.sidebar}, {"button", c.button},
        {"hover", c.hover}, {"hoverSoft", c.hoverSoft}, {"press", c.press}, {"field", c.field}, {"border", c.border},
        {"tabPill", c.tabPill}, {"tabActive", c.tabActive}, {"tabHome", c.tabHome}, {"segTrack", c.segTrack}, {"segOn", c.segOn}, {"scroll", c.scroll},
        {"scrollHover", c.scrollHover}, {"solid", c.solid}, {"solidBorder", c.solidBorder}, {"accent", c.accent},
        {"accentHover", c.accentHover}, {"accentPress", c.accentPress}, {"accentText", c.accentText},
        {"green", c.green}, {"orange", c.orange}, {"red", c.red}, {"overlay", c.overlay},
        {"overlayText", c.overlayText}, {"overlayText2", c.overlayText2}
    };
    QString sheet = QString::fromUtf8(kStyleSheet);
    for (auto it = tokens.cbegin(); it != tokens.cend(); ++it)
        sheet.replace(QLatin1Char('@') + it.key() + QLatin1Char('@'), css(it.value()));
    app.setStyleSheet(sheet);
}

} // namespace

namespace Theme {

void apply(QApplication &app)
{
    readMode();
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    // Soft text: no hinting, so letters are not snapped to the pixel grid.
    QFont base(QStringLiteral("Segoe UI"));
    base.setPixelSize(13);
    base.setHintingPreference(QFont::PreferNoHinting);
    base.setStyleStrategy(QFont::PreferAntialias);
    app.setFont(base);
    applyMode(app);

    app.setEffectEnabled(Qt::UI_AnimateCombo, false);
    app.setEffectEnabled(Qt::UI_FadeMenu, false);
    app.setEffectEnabled(Qt::UI_FadeTooltip, false);
}

Mode mode()
{
    readMode();
    return g_mode;
}

ThemeNotifier *notifier()
{
    static ThemeNotifier n;
    return &n;
}

void setMode(Mode m)
{
    readMode();
    if (m == g_mode)
        return;
    g_mode = m;
    QSettings().setValue(QStringLiteral("theme"), m == Light ? QStringLiteral("light") : QStringLiteral("dark"));
    applyMode(*qobject_cast<QApplication *>(QApplication::instance()));
    emit notifier()->changed();
}

QFont font(int pixelSize, QFont::Weight weight)
{
    // Painted widgets ask for the same few fonts on every repaint; resolve each once.
    static QHash<int, QFont> cache;
    const int key = pixelSize * 1000 + int(weight);
    const auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return *it;
    QFont f;
    f.setFamily(QStringLiteral("Segoe UI"));
    f.setHintingPreference(QFont::PreferNoHinting);
    f.setStyleStrategy(QFont::PreferAntialias);
    f.setPixelSize(pixelSize);
    f.setWeight(weight);
    cache.insert(key, f);
    return f;
}

void darkTitleBar(QWidget *window)
{
    const BOOL on = g_mode == Dark ? TRUE : FALSE;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &on, sizeof(on));
}

} // namespace Theme
