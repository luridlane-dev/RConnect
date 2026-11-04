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
QWidget { color: @text@; font-family: "SF Pro Text", "Segoe UI Variable Text", "Segoe UI"; font-size: 13px; }
QWidget#root { background: @bg@; }
QMainWindow, QDialog, QMessageBox { background: @bg@; }

QWidget#topBar { background: @topBar@; border-bottom: 1px solid @border@; }
QToolButton#menuButton {
    background: transparent; border: none; border-radius: 0; color: @text2@; padding: 0 0 4px 0; margin: 6px 4px; font-size: 18px;
}
QToolButton#menuButton:hover { background: @hover@; color: @text@; }
QToolButton#menuButton:pressed { background: @press@; }
QToolButton#homeTab {
    background: transparent; border: none; border-radius: 0; padding: 0; margin: 6px 4px 6px 6px;
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
QFrame#popup { background: @solid@; border: 1px solid @solidBorder@; border-radius: 0; }
QLabel#brandName { font-size: 16px; font-weight: 700; color: @text@; }
QLabel#brandSummary { color: @text2@; font-size: 12px; }
QLabel#pageTitle { font-size: 26px; font-weight: 700; color: @text@; }
QLabel#pageSubtitle { color: @text2@; font-size: 13px; }
QLabel#operatorName { color: @text@; font-size: 12px; font-weight: 600; }
QFrame#sidebar { background: @sidebar@; border-right: 1px solid @border@; }
QFrame#detailCard { background: @surface@; border: 1px solid @border@; border-radius: 0; }
QFrame#statTile { background: @surface@; border: 1px solid @border@; border-radius: 0; }
QLabel#statValue { font-size: 26px; font-weight: 700; color: @text@; background: transparent; }
QLabel#statValue[tone="green"] { color: @green@; }
QLabel#statValue[tone="orange"] { color: @orange@; }
QLabel#statValue[tone="red"] { color: @red@; }
QLabel#statCaption { font-size: 12px; color: @text2@; background: transparent; }
QFrame#navPill { background: @tabPill@; border-radius: 0; }
QPushButton#navButton {
    background: transparent; border: none; border-radius: 0; color: @text2@; text-align: left;
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
    border: none; border-radius: 0; color: @text2@; padding: 0;
}
QToolButton#settingsButton:hover { background: @hover@; color: @text@; }
QToolButton#sectionToggle {
    background: transparent; border: none; color: @accentText@; padding: 2px 0; font-weight: 600;
}
QToolButton#sectionToggle:hover { color: @accent@; }
QPlainTextEdit {
    background: @field@; border: 1px solid @border@; border-radius: 0; padding: 5px 8px;
    selection-background-color: @accent@; selection-color: #ffffff;
}
QPlainTextEdit:focus { border-color: @accent@; }

QListView { background: transparent; border: none; outline: none; }
QTableView {
    background: @surface@; alternate-background-color: @surfaceAlt@; border: 1px solid @border@; border-radius: 0;
    gridline-color: transparent; outline: none; selection-background-color: @accent@; selection-color: #ffffff;
}
QTableView::item { padding: 3px 8px; border: none; }
QHeaderView::section {
    background: @surfaceAlt@; color: @text2@; border: none; border-bottom: 1px solid @border@; padding: 6px 8px;
    font-weight: 600;
}
QTableView QScrollBar:horizontal { height: 8px; margin: 2px; background: transparent; }
QTableView QScrollBar::handle:horizontal { background: @scroll@; border-radius: 0; min-width: 36px; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: @scroll@; border-radius: 0; min-height: 36px; }
QScrollBar::handle:vertical:hover { background: @scrollHover@; }
QScrollBar::handle:vertical:disabled { background: transparent; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QPushButton, QToolButton {
    background: @button@; border: 1px solid @border@; border-radius: 0; padding: 6px 14px;
}
QPushButton:hover, QToolButton:hover { background: @hover@; }
QPushButton:pressed, QToolButton:pressed { background: @press@; }
QPushButton:disabled { background: @surface@; color: @text3@; }
QPushButton#primaryButton { background: @accent@; border-color: transparent; color: #ffffff; font-weight: 600; }
QPushButton#primaryButton:disabled { background: @surface@; color: @text3@; }
QPushButton#primaryButton:hover { background: @accentHover@; }
QPushButton#primaryButton:pressed { background: @accentPress@; }
QPushButton#addButton {
    background: @accent@; border-color: transparent; color: #ffffff; font-weight: 600; border-radius: 0;
    padding: 0 18px; min-height: 36px;
}
QPushButton#addButton:hover { background: @accentHover@; }
QPushButton#addButton:pressed { background: @accentPress@; }

QFrame#segmented { background: @segTrack@; border-radius: 0; }
QPushButton#segment {
    background: transparent; color: @text2@; border: none; border-radius: 0; padding: 0 14px;
    min-height: 28px; font-weight: 500;
}
QPushButton#segment:hover { color: @text@; background: transparent; }
QPushButton#segment:checked { background: @segOn@; color: @text@; }

QLineEdit, QSpinBox, QComboBox {
    background: @field@; border: 1px solid @border@; border-radius: 0; padding: 6px 10px;
    selection-background-color: @accent@; selection-color: #ffffff;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border: 1px solid @accent@; }
QLineEdit#searchField { border-radius: 0; padding: 0 12px; min-height: 36px; }
QComboBox QAbstractItemView {
    background: @solid@; border: 1px solid @solidBorder@; border-radius: 0; selection-background-color: @accent@;
    selection-color: #ffffff; outline: none;
}
QCheckBox { spacing: 10px; }
QCheckBox::indicator { width: 16px; height: 16px; border-radius: 0; border: 1.5px solid @text3@; background: transparent; }
QCheckBox::indicator:checked { background: @accent@; border-color: @accent@; }

QTabBar { background: transparent; }
QTabBar::tab {
    background: transparent; color: @text2@; padding: 0 10px 0 22px; margin: 0; border: none;
    min-width: 90px; max-width: 200px;
}
QTabBar::tab:hover { color: @text@; }
QTabBar::tab:selected { color: @text@; background: transparent; }
QToolButton#tabClose {
    background: transparent; border: none; border-radius: 0; color: @text2@; font-size: 14px; padding: 0; margin: 0;
}
QToolButton#tabClose:hover { background: @hover@; color: @text@; }
QToolButton#tabClose:pressed { background: @press@; }

QWidget#emptyPage { background: transparent; }

QWidget#sessionOverlay { background: @overlay@; }
QLabel#overlayTitle { font-size: 22px; font-weight: 700; color: @overlayText@; }
QLabel#overlayMessage { color: @overlayText2@; font-size: 14px; }

QMenu { background: @solid@; border: 1px solid @solidBorder@; border-radius: 0; padding: 5px; }
QMenu::item { padding: 6px 22px; border-radius: 0; }
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
        {"bg", "#000000"}, {"bgTop", "#000000"}, {"topBar", "#000000"},
        {"text", "#e8e8e8"}, {"text2", "#8f8f8f"}, {"text3", "#5e5e5e"},
        {"surface", "rgba(255, 255, 255, 0.04)"}, {"surfaceAlt", "rgba(255, 255, 255, 0.025)"},
        {"sidebar", "rgba(255, 255, 255, 0.025)"},
        {"button", "rgba(255, 255, 255, 0.07)"}, {"hover", "rgba(255, 255, 255, 0.12)"},
        {"hoverSoft", "rgba(255, 255, 255, 0.06)"}, {"press", "rgba(255, 255, 255, 0.05)"},
        {"field", "rgba(255, 255, 255, 0.06)"}, {"border", "rgba(255, 255, 255, 0.10)"},
        {"tabPill", "rgba(255, 255, 255, 0.23)"},
        {"segTrack", "rgba(255, 255, 255, 0.06)"}, {"segOn", "rgba(255, 255, 255, 0.14)"},
        {"scroll", "rgba(255, 255, 255, 0.20)"}, {"scrollHover", "rgba(255, 255, 255, 0.40)"},
        {"solid", "#0b0b0b"}, {"solidBorder", "#2a2a2a"},
        {"accent", "#4a86c8"}, {"accentHover", "#5a95d6"}, {"accentPress", "#3b72b0"}, {"accentText", "#7da9dc"},
        {"green", "#6aa97d"}, {"orange", "#c8975a"}, {"red", "#c4675f"},
        {"overlay", "#000000"}, {"overlayText", "#e8e8e8"}, {"overlayText2", "#8f8f8f"},
    };
    p.bg = QColor(0x00, 0x00, 0x00);
    p.text = QColor(0xe8, 0xe8, 0xe8);
    p.text2 = QColor(0x8f, 0x8f, 0x8f);
    p.text3 = QColor(0x5e, 0x5e, 0x5e);
    p.accent = QColor(0x4a, 0x86, 0xc8);
    p.solid = QColor(0x0b, 0x0b, 0x0b);
    p.button = QColor(0x16, 0x16, 0x16);
    return p;
}

Palette lightPalette()
{
    Palette p;
    p.tokens = {
        {"bg", "#f3f3f3"}, {"bgTop", "#f3f3f3"}, {"topBar", "#ffffff"},
        {"text", "#1c1c1c"}, {"text2", "#6b6b6b"}, {"text3", "#9a9a9a"},
        {"surface", "#ffffff"}, {"surfaceAlt", "#fafafa"},
        {"sidebar", "#fafafa"},
        {"button", "#ffffff"}, {"hover", "rgba(0, 0, 0, 0.06)"},
        {"hoverSoft", "rgba(0, 0, 0, 0.04)"}, {"press", "rgba(0, 0, 0, 0.10)"},
        {"field", "#ffffff"}, {"border", "rgba(0, 0, 0, 0.10)"},
        {"tabPill", "rgba(0, 0, 0, 0.13)"},
        {"segTrack", "rgba(0, 0, 0, 0.06)"}, {"segOn", "#ffffff"},
        {"scroll", "rgba(0, 0, 0, 0.22)"}, {"scrollHover", "rgba(0, 0, 0, 0.42)"},
        {"solid", "#ffffff"}, {"solidBorder", "#d9d9d9"},
        {"accent", "#2f6fb5"}, {"accentHover", "#3f7fc5"}, {"accentPress", "#265c98"}, {"accentText", "#2f6fb5"},
        {"green", "#3f8a56"}, {"orange", "#a9702a"}, {"red", "#b5483f"},
        {"overlay", "#f3f3f3"}, {"overlayText", "#1c1c1c"}, {"overlayText2", "#6b6b6b"},
    };
    p.bg = QColor(0xf3, 0xf3, 0xf3);
    p.text = QColor(0x1c, 0x1c, 0x1c);
    p.text2 = QColor(0x6b, 0x6b, 0x6b);
    p.text3 = QColor(0x9a, 0x9a, 0x9a);
    p.accent = QColor(0x2f, 0x6f, 0xb5);
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
        kCard = QColor(0x0c, 0x0c, 0x0c);
        kCardHover = QColor(0x16, 0x16, 0x16);
        kCardPressed = QColor(0x10, 0x10, 0x10);
        kSeparator = QColor(255, 255, 255, 22);
        kSeparatorStrong = QColor(255, 255, 255, 50);
        kLabel = QColor(0xe8, 0xe8, 0xe8);
        kLabelSecondary = QColor(0x8f, 0x8f, 0x8f);
        kLabelTertiary = QColor(0x5e, 0x5e, 0x5e);
        kBlue = QColor(0x4a, 0x86, 0xc8);
        kBlueText = QColor(0x7d, 0xa9, 0xdc);
        kGreen = QColor(0x6a, 0xa9, 0x7d);
        kOrange = QColor(0xc8, 0x97, 0x5a);
        kRed = QColor(0xc4, 0x67, 0x5f);
        kBusy = QColor(0xff, 0x5a, 0x4d);
        kTabPill = QColor(255, 255, 255, 58);
        kBarOff = QColor(255, 255, 255, 40);
        kPopupFill = QColor(0x0b, 0x0b, 0x0b);
        kPopupBorder = QColor(0x2a, 0x2a, 0x2a);
        kIconLine = QColor(0xd1, 0xd1, 0xd6);
    } else {
        kBackground = QColor(0xf3, 0xf3, 0xf3);
        kCard = QColor(0xff, 0xff, 0xff);
        kCardHover = QColor(0xf8, 0xf8, 0xf8);
        kCardPressed = QColor(0xec, 0xec, 0xec);
        kSeparator = QColor(0, 0, 0, 22);
        kSeparatorStrong = QColor(0, 0, 0, 56);
        kLabel = QColor(0x1c, 0x1c, 0x1c);
        kLabelSecondary = QColor(0x6b, 0x6b, 0x6b);
        kLabelTertiary = QColor(0x9a, 0x9a, 0x9a);
        kBlue = QColor(0x2f, 0x6f, 0xb5);
        kBlueText = QColor(0x2f, 0x6f, 0xb5);
        kGreen = QColor(0x3f, 0x8a, 0x56);
        kOrange = QColor(0xa9, 0x70, 0x2a);
        kRed = QColor(0xb5, 0x48, 0x3f);
        kBusy = QColor(0xd9, 0x3a, 0x2e);
        kTabPill = QColor(0, 0, 0, 34);
        kBarOff = QColor(0, 0, 0, 36);
        kPopupFill = QColor(0xff, 0xff, 0xff);
        kPopupBorder = QColor(0xd9, 0xd9, 0xd9);
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
    p.setColor(QPalette::Base, dark ? QColor(0x0a, 0x0a, 0x0a) : QColor(0xff, 0xff, 0xff));
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
