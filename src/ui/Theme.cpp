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
* { font-family: "SF Pro Text", "Segoe UI Variable Text", "Segoe UI"; font-size: 13px; }
QWidget { color: @text@; }
QWidget#root {
    background: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 @bgTop@, stop: 1 @bg@);
}
QMainWindow, QDialog, QMessageBox { background: @bg@; }

QWidget#topBar { background: @topBar@; border-bottom: 1px solid @border@; }
QToolButton#menuButton {
    background: transparent; border: none; border-radius: 9px; color: @text2@; padding: 0 0 4px 0; margin: 6px 4px; font-size: 18px;
}
QToolButton#menuButton:hover { background: @hover@; color: @text@; }
QToolButton#menuButton:pressed { background: @press@; }
QToolButton#homeTab {
    background: transparent; border: none; border-radius: 9px; padding: 0; margin: 6px 4px 6px 6px;
    min-width: 34px; max-width: 34px;
}
QToolButton#homeTab:hover { background: @hover@; }
QToolButton#homeTab:checked { background: @tabPill@; }
QToolButton#winButton {
    background: transparent; border: none; border-radius: 0; color: @text2@; padding: 0; font-size: 13px;
}
QToolButton#winButton:hover { background: @hover@; color: @text@; }
QToolButton#winClose {
    background: transparent; border: none; border-radius: 0; color: @text2@; padding: 0; font-size: 15px;
}
QToolButton#winClose:hover { background: #ff453a; color: #ffffff; }
QToolButton#trafficClose, QToolButton#trafficMin, QToolButton#trafficMax {
    border: none; border-radius: 6px; padding: 0; min-width: 12px; max-width: 12px; min-height: 12px; max-height: 12px;
}
QToolButton#trafficClose { background: #ff5f57; }
QToolButton#trafficMin { background: #febc2e; }
QToolButton#trafficMax { background: #28c840; }
QToolButton#trafficClose:hover { background: #ff8780; }
QToolButton#trafficMin:hover { background: #ffd060; }
QToolButton#trafficMax:hover { background: #5bdc70; }
QFrame#popup { background: @solid@; border: 1px solid @solidBorder@; border-radius: 12px; }
QLabel#brandName { font-size: 16px; font-weight: 700; color: @text@; }
QLabel#brandSummary { color: @text2@; font-size: 12px; }
QLabel#pageTitle { font-size: 26px; font-weight: 700; color: @text@; }
QLabel#pageSubtitle { color: @text2@; font-size: 13px; }
QLabel#operatorName { color: @text@; font-size: 12px; font-weight: 600; }
QFrame#sidebar { background: @sidebar@; border-right: 1px solid @border@; }
QFrame#detailCard { background: @surface@; border: 1px solid @border@; border-radius: 12px; }
QFrame#statTile { background: @surface@; border: 1px solid @border@; border-radius: 14px; }
QLabel#statValue { font-size: 26px; font-weight: 700; color: @text@; background: transparent; }
QLabel#statValue[tone="green"] { color: @green@; }
QLabel#statValue[tone="orange"] { color: @orange@; }
QLabel#statValue[tone="red"] { color: @red@; }
QLabel#statCaption { font-size: 12px; color: @text2@; background: transparent; }
QFrame#navPill { background: @tabPill@; border-radius: 9px; }
QPushButton#navButton {
    background: transparent; border: none; border-radius: 9px; color: @text2@; text-align: left;
    padding: 7px 12px; font-size: 14px; font-weight: 500;
}
QPushButton#navButton:hover { background: @hoverSoft@; color: @text@; }
QPushButton#navButton:checked { background: transparent; color: @text@; font-weight: 600; }
QLabel#emptyState { color: @text3@; font-size: 14px; }
QLabel#popupTitle { font-size: 13px; font-weight: 600; color: @text@; }
QLabel#fieldHint { color: @text2@; font-size: 12px; }
QToolButton#barButton {
    background: transparent; border: none; border-radius: 0; color: @text2@; padding: 0 12px; font-size: 13px;
}
QToolButton#barButton:hover { background: @hover@; color: @text@; }
QToolButton#settingsButton {
    font-family: "Segoe Fluent Icons", "Segoe MDL2 Assets"; font-size: 16px; background: @button@;
    border: none; border-radius: 7px; color: @text2@; padding: 0;
}
QToolButton#settingsButton:hover { background: @hover@; color: @text@; }
QToolButton#sectionToggle {
    background: transparent; border: none; color: @accentText@; padding: 2px 0; font-weight: 600;
}
QToolButton#sectionToggle:hover { color: @accent@; }
QPlainTextEdit {
    background: @field@; border: 1px solid @border@; border-radius: 9px; padding: 5px 8px;
    selection-background-color: @accent@; selection-color: #ffffff;
}
QPlainTextEdit:focus { border-color: @accent@; }

QListView { background: transparent; border: none; outline: none; }
QTableView {
    background: @surface@; alternate-background-color: @surfaceAlt@; border: 1px solid @border@; border-radius: 12px;
    gridline-color: transparent; outline: none; selection-background-color: @accent@; selection-color: #ffffff;
}
QTableView::item { padding: 3px 8px; border: none; }
QHeaderView::section {
    background: @surfaceAlt@; color: @text2@; border: none; border-bottom: 1px solid @border@; padding: 6px 8px;
    font-weight: 600;
}
QTableView QScrollBar:horizontal { height: 8px; margin: 2px; background: transparent; }
QTableView QScrollBar::handle:horizontal { background: @scroll@; border-radius: 3px; min-width: 36px; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: @scroll@; border-radius: 3px; min-height: 36px; }
QScrollBar::handle:vertical:hover { background: @scrollHover@; }
QScrollBar::handle:vertical:disabled { background: transparent; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QPushButton, QToolButton {
    background: @button@; border: 1px solid @border@; border-radius: 8px; padding: 6px 14px;
}
QPushButton:hover, QToolButton:hover { background: @hover@; }
QPushButton:pressed, QToolButton:pressed { background: @press@; }
QPushButton:disabled { background: @surface@; color: @text3@; }
QPushButton#primaryButton { background: @accent@; border-color: transparent; color: #ffffff; font-weight: 600; }
QPushButton#primaryButton:disabled { background: @surface@; color: @text3@; }
QPushButton#primaryButton:hover { background: @accentHover@; }
QPushButton#primaryButton:pressed { background: @accentPress@; }
QPushButton#addButton {
    background: @accent@; border-color: transparent; color: #ffffff; font-weight: 600; border-radius: 9px;
    padding: 0 18px; min-height: 36px;
}
QPushButton#addButton:hover { background: @accentHover@; }
QPushButton#addButton:pressed { background: @accentPress@; }

QFrame#segmented { background: @segTrack@; border-radius: 10px; }
QPushButton#segment {
    background: transparent; color: @text2@; border: none; border-radius: 8px; padding: 0 14px;
    min-height: 28px; font-weight: 500;
}
QPushButton#segment:hover { color: @text@; background: transparent; }
QPushButton#segment:checked { background: @segOn@; color: @text@; }

QLineEdit, QSpinBox, QComboBox {
    background: @field@; border: 1px solid @border@; border-radius: 9px; padding: 6px 10px;
    selection-background-color: @accent@; selection-color: #ffffff;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border: 1px solid @accent@; }
QLineEdit#searchField { border-radius: 10px; padding: 0 12px; min-height: 36px; }
QComboBox QAbstractItemView {
    background: @solid@; border: 1px solid @solidBorder@; border-radius: 8px; selection-background-color: @accent@;
    selection-color: #ffffff; outline: none;
}
QCheckBox { spacing: 10px; }
QCheckBox::indicator { width: 16px; height: 16px; border-radius: 5px; border: 1.5px solid @text3@; background: transparent; }
QCheckBox::indicator:checked { background: @accent@; border-color: @accent@; }

QTabBar { background: transparent; }
QTabBar::tab {
    background: transparent; color: @text2@; padding: 0 10px 2px 10px; margin: 0; border: none;
    min-width: 90px; max-width: 200px;
}
QTabBar::tab:hover { color: @text@; }
QTabBar::tab:selected { color: @text@; background: transparent; }
QToolButton#tabClose {
    background: transparent; border: none; border-radius: 9px; color: @text2@; font-size: 14px; padding: 0; margin: 0;
}
QToolButton#tabClose:hover { background: @hover@; color: @text@; }
QToolButton#tabClose:pressed { background: @press@; }

QWidget#emptyPage { background: transparent; }

QWidget#sessionOverlay { background: @overlay@; }
QLabel#overlayTitle { font-size: 22px; font-weight: 700; color: @overlayText@; }
QLabel#overlayMessage { color: @overlayText2@; font-size: 14px; }

QMenu { background: @solid@; border: 1px solid @solidBorder@; border-radius: 10px; padding: 5px; }
QMenu::item { padding: 6px 22px; border-radius: 6px; }
QMenu::item:selected { background: @accent@; color: #ffffff; }
QMenu::separator { height: 1px; background: @solidBorder@; margin: 4px 8px; }
QToolTip { background: @solid@; color: @text@; border: 1px solid @solidBorder@; padding: 5px 9px; font-size: 12px; }
)";

struct Palette {
    QHash<QString, QString> tokens;
    QColor bg, text, text2, text3, accent, solid, button;
};

Palette darkPalette()
{
    Palette p;
    p.tokens = {
        {"bg", "#000000"}, {"bgTop", "#101014"}, {"topBar", "rgba(255, 255, 255, 0.04)"},
        {"text", "#f5f5f7"}, {"text2", "#a1a1a6"}, {"text3", "#6e6e73"},
        {"surface", "rgba(255, 255, 255, 0.05)"}, {"surfaceAlt", "rgba(255, 255, 255, 0.03)"},
        {"sidebar", "rgba(255, 255, 255, 0.03)"},
        {"button", "rgba(255, 255, 255, 0.09)"}, {"hover", "rgba(255, 255, 255, 0.14)"},
        {"hoverSoft", "rgba(255, 255, 255, 0.07)"}, {"press", "rgba(255, 255, 255, 0.06)"},
        {"field", "rgba(255, 255, 255, 0.07)"}, {"border", "rgba(255, 255, 255, 0.10)"},
        {"tabPill", "rgba(255, 255, 255, 0.11)"},
        {"segTrack", "rgba(255, 255, 255, 0.07)"}, {"segOn", "rgba(255, 255, 255, 0.18)"},
        {"scroll", "rgba(255, 255, 255, 0.25)"}, {"scrollHover", "rgba(255, 255, 255, 0.45)"},
        {"solid", "#16161a"}, {"solidBorder", "#2c2c32"},
        {"accent", "#0a84ff"}, {"accentHover", "#409cff"}, {"accentPress", "#0066d6"}, {"accentText", "#64a8ff"},
        {"green", "#30d158"}, {"orange", "#ff9f0a"}, {"red", "#ff453a"},
        {"overlay", "#000000"}, {"overlayText", "#ffffff"}, {"overlayText2", "#a1a1a6"},
    };
    p.bg = QColor(0x00, 0x00, 0x00);
    p.text = QColor(0xf5, 0xf5, 0xf7);
    p.text2 = QColor(0xa1, 0xa1, 0xa6);
    p.text3 = QColor(0x6e, 0x6e, 0x73);
    p.accent = QColor(0x0a, 0x84, 0xff);
    p.solid = QColor(0x16, 0x16, 0x1a);
    p.button = QColor(0x24, 0x24, 0x28);
    return p;
}

Palette lightPalette()
{
    Palette p;
    p.tokens = {
        {"bg", "#e9ebf1"}, {"bgTop", "#f7f8fb"}, {"topBar", "rgba(255, 255, 255, 0.55)"},
        {"text", "#1d1d1f"}, {"text2", "#5c5c63"}, {"text3", "#8e8e93"},
        {"surface", "rgba(255, 255, 255, 0.68)"}, {"surfaceAlt", "rgba(255, 255, 255, 0.40)"},
        {"sidebar", "rgba(255, 255, 255, 0.45)"},
        {"button", "rgba(255, 255, 255, 0.85)"}, {"hover", "rgba(0, 0, 0, 0.07)"},
        {"hoverSoft", "rgba(0, 0, 0, 0.045)"}, {"press", "rgba(0, 0, 0, 0.11)"},
        {"field", "rgba(255, 255, 255, 0.85)"}, {"border", "rgba(0, 0, 0, 0.10)"},
        {"tabPill", "rgba(255, 255, 255, 0.95)"},
        {"segTrack", "rgba(0, 0, 0, 0.07)"}, {"segOn", "#ffffff"},
        {"scroll", "rgba(0, 0, 0, 0.25)"}, {"scrollHover", "rgba(0, 0, 0, 0.45)"},
        {"solid", "#ffffff"}, {"solidBorder", "#d2d4db"},
        {"accent", "#0071e3"}, {"accentHover", "#0a84ff"}, {"accentPress", "#0058b8"}, {"accentText", "#0066cc"},
        {"green", "#1f9d3c"}, {"orange", "#c76a00"}, {"red", "#d92d20"},
        {"overlay", "#f3f4f8"}, {"overlayText", "#1d1d1f"}, {"overlayText2", "#5c5c63"},
    };
    p.bg = QColor(0xe9, 0xeb, 0xf1);
    p.text = QColor(0x1d, 0x1d, 0x1f);
    p.text2 = QColor(0x5c, 0x5c, 0x63);
    p.text3 = QColor(0x8e, 0x8e, 0x93);
    p.accent = QColor(0x00, 0x71, 0xe3);
    p.solid = QColor(0xff, 0xff, 0xff);
    p.button = QColor(0xff, 0xff, 0xff);
    return p;
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

void setColours(bool dark)
{
    using namespace Theme;
    if (dark) {
        kBackground = QColor(0x00, 0x00, 0x00);
        kCard = QColor(255, 255, 255, 15);
        kCardHover = QColor(255, 255, 255, 27);
        kCardPressed = QColor(255, 255, 255, 20);
        kSeparator = QColor(255, 255, 255, 26);
        kSeparatorStrong = QColor(255, 255, 255, 56);
        kLabel = QColor(0xf5, 0xf5, 0xf7);
        kLabelSecondary = QColor(0xa1, 0xa1, 0xa6);
        kLabelTertiary = QColor(0x6e, 0x6e, 0x73);
        kBlue = QColor(0x0a, 0x84, 0xff);
        kBlueText = QColor(0x64, 0xa8, 0xff);
        kGreen = QColor(0x30, 0xd1, 0x58);
        kOrange = QColor(0xff, 0x9f, 0x0a);
        kRed = QColor(0xff, 0x45, 0x3a);
        kTabPill = QColor(255, 255, 255, 28);
        kBarOff = QColor(255, 255, 255, 46);
        kPopupFill = QColor(0x16, 0x16, 0x1a);
        kPopupBorder = QColor(0x2c, 0x2c, 0x32);
        kIconLine = QColor(0xd1, 0xd1, 0xd6);
    } else {
        kBackground = QColor(0xe9, 0xeb, 0xf1);
        kCard = QColor(255, 255, 255, 175);
        kCardHover = QColor(255, 255, 255, 235);
        kCardPressed = QColor(255, 255, 255, 140);
        kSeparator = QColor(0, 0, 0, 26);
        kSeparatorStrong = QColor(0, 0, 0, 56);
        kLabel = QColor(0x1d, 0x1d, 0x1f);
        kLabelSecondary = QColor(0x5c, 0x5c, 0x63);
        kLabelTertiary = QColor(0x8e, 0x8e, 0x93);
        kBlue = QColor(0x00, 0x71, 0xe3);
        kBlueText = QColor(0x00, 0x66, 0xcc);
        kGreen = QColor(0x1f, 0x9d, 0x3c);
        kOrange = QColor(0xc7, 0x6a, 0x00);
        kRed = QColor(0xd9, 0x2d, 0x20);
        kTabPill = QColor(255, 255, 255, 240);
        kBarOff = QColor(0, 0, 0, 36);
        kPopupFill = QColor(0xff, 0xff, 0xff);
        kPopupBorder = QColor(0xd2, 0xd4, 0xdb);
        kIconLine = QColor(0x3a, 0x3a, 0x3f);
    }
}

void applyMode(QApplication &app)
{
    const bool dark = g_mode == Theme::Dark;
    const Palette pal = dark ? darkPalette() : lightPalette();
    setColours(dark);

    QPalette p;
    p.setColor(QPalette::Window, pal.bg);
    p.setColor(QPalette::WindowText, pal.text);
    p.setColor(QPalette::Base, dark ? QColor(0x1c, 0x1c, 0x20) : QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::AlternateBase, pal.bg);
    p.setColor(QPalette::Text, pal.text);
    p.setColor(QPalette::Button, pal.button);
    p.setColor(QPalette::ButtonText, pal.text);
    p.setColor(QPalette::ToolTipBase, pal.solid);
    p.setColor(QPalette::ToolTipText, pal.text);
    p.setColor(QPalette::Highlight, pal.accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, pal.text2);
    p.setColor(QPalette::Disabled, QPalette::Text, pal.text3);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, pal.text3);
    app.setPalette(p);

    QString sheet = QString::fromUtf8(kStyleSheet);
    for (auto it = pal.tokens.cbegin(); it != pal.tokens.cend(); ++it)
        sheet.replace(QLatin1Char('@') + it.key() + QLatin1Char('@'), it.value());
    app.setStyleSheet(sheet);
}

} // namespace

namespace Theme {

void apply(QApplication &app)
{
    readMode();
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
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
    QFont f;
    f.setFamilies({QStringLiteral("SF Pro Text"), QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI")});
    f.setPixelSize(pixelSize);
    f.setWeight(weight);
    return f;
}

void darkTitleBar(QWidget *window)
{
    const BOOL on = g_mode == Dark ? TRUE : FALSE;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &on, sizeof(on));
}

} // namespace Theme
