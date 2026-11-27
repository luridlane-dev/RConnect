#include "Theme.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
#include <QWidget>

#include <windows.h>
#include <dwmapi.h>

namespace {

// Tokens in @name@ are replaced by the colours and font families of the current mode.
const char *const kStyleSheet = R"(
QWidget { color: @text@; font-family: "@fontFamily@", "Segoe UI"; font-size: 13px; }
QWidget#root, QMainWindow, QDialog, QMessageBox { background: @bg@; }

QWidget#topBar { background: @topBar@; border: none; border-bottom: 1px solid @hairline@; }
QToolButton#menuButton, QToolButton#homeTab, QToolButton#barButton {
    background: transparent; border: none; border-radius: 0px; color: @text2@; margin: 4px 3px; padding: 0;
}
QToolButton#menuButton { font-size: 19px; padding-bottom: 2px; }
QToolButton#barButton { padding: 0 12px; }
QToolButton#menuButton:hover, QToolButton#homeTab:hover, QToolButton#barButton:hover {
    background: @hover@; color: @text@;
}
QToolButton#homeTab:checked {
    background: @tabActive@; border-bottom: 2px solid @tabActiveAccent@; color: @tabActiveText@;
}
QToolButton#homeTab:checked:hover { background: @hover@; }
QToolButton#winButton, QToolButton#winClose {
    background: transparent; border: none; border-radius: 0px; color: @text2@; padding: 0; font-size: 14px;
}
QToolButton#winButton:hover { background: @hover@; color: @text@; }
QToolButton#winClose:hover { background: #e5484d; color: #ffffff; }
QToolButton#trafficClose, QToolButton#trafficMin, QToolButton#trafficMax {
    border: none; border-radius: 6px; padding: 0; min-width: 12px; max-width: 12px; min-height: 12px; max-height: 12px;
}
QToolButton#trafficClose { background: #ff5f57; }
QToolButton#trafficMin { background: #febc2e; }
QToolButton#trafficMax { background: #28c840; }
QToolButton#trafficClose:hover { background: #ff8780; }
QToolButton#trafficMin:hover { background: #ffd060; }
QToolButton#trafficMax:hover { background: #5bdc70; }

QLabel#brandName { font-family: "@displayFontFamily@", "Segoe UI"; font-size: 16px; font-weight: 600; }
QLabel#pageTitle { font-family: "@displayFontFamily@", "Segoe UI"; font-size: 24px; font-weight: 600; }
QLabel#metricValue { font-family: "@displayFontFamily@", "Segoe UI"; font-size: 20px; font-weight: 600; }
QLabel#pageSubtitle, QLabel#fieldHint { color: @text2@; }
QLabel#fieldHint { font-size: 12px; }
QLabel#settingsHeading, QLabel#popupTitle { font-size: 13px; font-weight: 600; color: @text2@; }
QLabel#settingsTitle { font-family: "@displayFontFamily@", "Segoe UI"; font-size: 21px; font-weight: 600; }
QFrame#settingsRail { background: @sidebar@; border: none; border-right: 1px solid @hairline@; }
QFrame#settingsFooter { background: @sidebar@; border: none; border-top: 1px solid @hairline@; }
QFrame#settingsSection {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @glassTop@, stop:1 @glassBottom@);
    border: none; border-radius: 0px;
}
QListWidget#settingsNavigation { background: transparent; border: none; outline: none; }
QListWidget#settingsNavigation::item {
    min-height: 20px; color: @text2@; margin: 2px 0; padding: 8px 12px;
    border: none; border-radius: 0px;
}
QListWidget#settingsNavigation::item:hover { background: @hoverSoft@; color: @text@; }
QListWidget#settingsNavigation::item:selected {
    background: @accentSoft@; color: @accentText@; font-weight: 600;
}
QLabel#emptyState { color: @text2@; font-size: 14px; }
QLabel#warningText { color: @orange@; }

QFrame#sidebar { background: @sidebar@; border: none; border-right: 1px solid @hairline@; }
QFrame#detailCard {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @glassTop@, stop:1 @glassBottom@);
    border: none; border-radius: 0px;
}
QFrame#popup { background: @solid@; border: 1px solid @solidBorder@; border-radius: 0px; }
QFrame#navPill { background: @accentSoft@; border: none; border-radius: 0px; }
QPushButton#navButton {
    background: transparent; border: none; border-radius: 0px; color: @text2@;
    text-align: left; min-height: 24px; padding: 8px 12px; font-weight: 500;
}
QPushButton#navButton:hover { background: @hoverSoft@; color: @text@; }
QPushButton#navButton:pressed { background: @hover@; }
QPushButton#navButton:checked { background: transparent; color: @accentText@; font-weight: 600; }

QPlainTextEdit, QLineEdit, QSpinBox, QComboBox {
    background: @field@; border: 1px solid transparent; border-radius: 0px; padding: 6px 11px;
    selection-background-color: @accent@; selection-color: @onAccent@;
}
QPlainTextEdit:hover, QLineEdit:hover, QSpinBox:hover, QComboBox:hover { background: @hover@; }
QPlainTextEdit:focus, QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: @focus@; }
QLineEdit#searchField { padding: 0 11px; min-height: 38px; border-radius: 0px; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox QAbstractItemView {
    background: @solid@; border: 1px solid @solidBorder@; border-radius: 0px; padding: 2px;
    selection-background-color: @selectionBg@; selection-color: @selectionText@; outline: none;
}
QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 16px; height: 16px; border-radius: 0px; }
QCheckBox::indicator:unchecked { background: @field@; border: 1px solid @solidBorder@; }
QCheckBox::indicator:unchecked:hover { background: @hover@; }
QCheckBox::indicator:checked { background: @accent@; border: 1px solid @accent@; }

QListView { background: transparent; border: none; outline: none; }
QTableView, QTreeView {
    background: @surface@; alternate-background-color: @surfaceAlt@; border: none; border-radius: 0px;
    outline: none; selection-background-color: @selectionBg@; selection-color: @selectionText@;
    show-decoration-selected: 1;
}
QTableView { gridline-color: transparent; }
QTableView::item { padding: 5px 10px; border: none; }
QTreeView::item { padding: 8px 6px; border: none; }
QTableView::item:hover { background: @rowHover@; }
QTableView::item:selected { background: @selectionBg@; color: @selectionText@; }
QTableView::item:selected:hover { background: @selectionHover@; color: @selectionText@; }
QTreeView::item:selected { background: transparent; color: @selectionText@; }
QHeaderView, QTableCornerButton::section { background: @surfaceAlt@; border: none; }
QHeaderView::section {
    background: @surfaceAlt@; color: @text2@; border: none; border-bottom: 1px solid @hairline@;
    padding: 9px 10px; font-size: 12px; font-weight: 600;
}
QAbstractScrollArea::corner { background: @surface@; }
QScrollBar:horizontal { height: 7px; margin: 2px; background: transparent; }
QScrollBar:vertical { width: 7px; margin: 2px; background: transparent; }
QScrollBar::handle { background: @scroll@; border-radius: 0px; }
QScrollBar::handle:horizontal { min-width: 32px; }
QScrollBar::handle:vertical { min-height: 32px; }
QScrollBar::handle:hover { background: @scrollHover@; }
QScrollBar::handle:disabled { background: transparent; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QPushButton, QToolButton {
    background: @button@; border: none; border-radius: 0px; padding: 7px 14px; font-weight: 500;
}
QPushButton:hover, QToolButton:hover { background: @hover@; }
QPushButton:pressed, QToolButton:pressed { background: @press@; }
QPushButton:focus, QToolButton:focus, QPushButton#navButton:focus, QPushButton#segment:focus {
    outline: none;
}
QPushButton#primaryButton, QPushButton#addButton {
    background: @accent@; border: none; color: @onAccent@; font-weight: 600;
}
QPushButton#primaryButton:hover, QPushButton#addButton:hover { background: @accentHover@; }
QPushButton#primaryButton:pressed, QPushButton#addButton:pressed { background: @accentPress@; }
QPushButton#pillToggle {
    background: @button@; border: none; border-radius: 0px; padding: 6px 13px; color: @text2@; font-size: 12px;
}
QPushButton#pillToggle:hover { background: @hover@; color: @text@; }
QPushButton#pillToggle:checked { background: @accentSoft@; color: @accentText@; font-weight: 600; }
QPushButton:disabled, QToolButton:disabled, QPushButton#primaryButton:disabled, QPushButton#addButton:disabled {
    background: @surfaceAlt@; color: @text3@;
}
QPushButton#addButton { padding: 0 18px; min-height: 38px; border-radius: 0px; }
QToolButton#sectionToggle {
    background: transparent; border: none; color: @accentText@; padding: 4px 6px; font-weight: 600;
}
QToolButton#sectionToggle:hover { background: @hoverSoft@; }

QFrame#segmented { background: @surface@; border: none; border-radius: 0px; }
QPushButton#segment {
    background: transparent; color: @text2@; border: none; border-radius: 0px;
    padding: 0 11px; min-height: 27px; font-weight: 500;
}
QPushButton#segment:hover { color: @text@; background: @hoverSoft@; }
QPushButton#segment:pressed { background: @hover@; color: @text@; }
QPushButton#segment:checked { background: @accentSoft@; color: @accentText@; font-weight: 600; }
QPushButton#segment:checked:pressed { background: @press@; }
QPushButton#segment[iconOnly="true"] { padding: 0; }

QTabBar { background: transparent; }
QTabBar::tab {
    background: transparent; color: @tabInactiveText@; padding: 0 6px 0 13px; margin: 4px 2px;
    border: none; border-radius: 0px; font-size: 12px; min-width: 80px; max-width: 185px;
}
QTabBar::tab:hover { color: @text@; background: @hoverSoft@; }
QTabBar::tab:selected { background: transparent; color: @tabInactiveText@; }
QTabBar[selectionVisible="true"]::tab:selected { background: transparent; color: @tabActiveText@; font-weight: 600; }
QTabBar[selectionVisible="true"]::tab:selected:hover { background: transparent; color: @tabActiveText@; }
QToolButton#tabClose {
    background: transparent; border: none; border-radius: 0px; color: @text2@; font-size: 13px; padding: 0;
}
QToolButton#tabClose:hover { background: @hover@; color: @text@; }

QWidget#emptyPage, QScrollArea#settingsScroll, QWidget#settingsContent { background: transparent; border: none; }
QWidget#sessionOverlay { background: @bg@; }
QLabel#overlayTitle { font-family: "@displayFontFamily@", "Segoe UI"; font-size: 22px; font-weight: 600; }
QLabel#overlayMessage { color: @text2@; font-size: 14px; }

QMenu { background: @solid@; border: 1px solid @solidBorder@; border-radius: 0px; padding: 4px; }
QMenu::item { padding: 7px 18px; border-radius: 0px; }
QMenu::item:selected { background: @selectionBg@; color: @selectionText@; }
QMenu::item:disabled { color: @text3@; }
QMenu::separator { height: 1px; background: @hairline@; margin: 4px 8px; }
QToolTip {
    background: @solid@; color: @text@; border: 1px solid @solidBorder@; border-radius: 0px;
    padding: 7px 11px; font-size: 12px;
}
)";

struct Scheme {
    QColor bg, topBar, surface, surfaceAlt, glassTop, glassBottom, sidebar, button, field;
    QColor hairline, focus, solid, solidBorder;
    QColor text, text2, text3;
    QColor hover, hoverSoft, rowHover, press, tabActive, tabActiveBorder, tabActiveAccent, tabActiveText, tabInactiveText, scroll, scrollHover;
    QColor accent, accentHover, accentPress, accentText, accentSoft, selectionBg, selectionHover, selectionText, onAccent;
    QColor green, teal, purple, orange, favorite, red, busy, busyText;
    QColor busySurface, connectedSurface, separator, separatorStrong, barOff;
};

Scheme darkScheme()
{
    Scheme c;
    // Pure OLED true-black glass theme: sharp borderless surfaces, neutral dark glass, calm Apple System Blue accent.
    c.bg = QColor("#060607");
    c.topBar = QColor("#0a0a0b");
    c.surface = QColor("#0f0f11");
    c.surfaceAlt = QColor("#0c0c0e");
    c.glassTop = QColor("#151518");
    c.glassBottom = QColor("#0f0f11");
    c.sidebar = QColor("#09090a");
    c.button = QColor("#17171a");
    c.field = QColor("#121215");
    c.hairline = QColor(255, 255, 255, 16);
    c.focus = QColor("#0a84ff");
    c.solid = QColor("#111114");
    c.solidBorder = QColor(255, 255, 255, 26);
    c.text = QColor("#f4f4f6");
    c.text2 = QColor("#9898a0");
    c.text3 = QColor("#63636c");
    c.hover = QColor("#1f1f24");
    c.hoverSoft = QColor(255, 255, 255, 14);
    c.rowHover = QColor("#17171b");
    c.press = QColor("#28282e");
    c.tabActive = QColor("#242936");
    c.tabActiveBorder = QColor("#3e475c");
    c.tabActiveAccent = QColor("#2e96ff");
    c.tabActiveText = QColor("#ffffff");
    c.tabInactiveText = QColor("#7c808c");
    c.scroll = QColor("#2d2d33");
    c.scrollHover = QColor("#45454e");
    c.accent = QColor("#0a84ff");
    c.accentHover = QColor("#2e96ff");
    c.accentPress = QColor("#0070e0");
    c.accentText = QColor("#52a5ff");
    c.accentSoft = QColor("#132236");
    c.selectionBg = QColor("#142336");
    c.selectionHover = QColor("#192c44");
    c.selectionText = QColor("#f4f4f6");
    c.onAccent = QColor("#ffffff");
    c.green = QColor("#34c77b");
    c.teal = QColor("#4fb8e8");
    c.purple = QColor("#9683f5");
    c.orange = QColor("#d99a36");
    c.favorite = QColor("#c9a23f");
    c.red = QColor("#e5534b");
    c.busy = QColor("#8b7cf6");
    c.busyText = QColor("#c4bcff");
    c.busySurface = QColor("#15141f");
    c.connectedSurface = QColor("#0e1a13");
    c.separator = QColor(255, 255, 255, 16);
    c.separatorStrong = QColor(255, 255, 255, 26);
    c.barOff = QColor("#26262b");
    return c;
}

Scheme lightScheme()
{
    Scheme c;
    c.bg = QColor("#f2f2f5");
    c.topBar = QColor("#ffffff");
    c.surface = QColor("#ffffff");
    c.surfaceAlt = QColor("#f7f7f9");
    c.glassTop = QColor("#ffffff");
    c.glassBottom = QColor("#fafafc");
    c.sidebar = QColor("#ebebef");
    c.button = QColor("#ffffff");
    c.field = QColor("#ffffff");
    c.hairline = QColor(15, 23, 42, 18);
    c.focus = QColor("#007aff");
    c.solid = QColor("#ffffff");
    c.solidBorder = QColor(15, 23, 42, 26);
    c.text = QColor("#161922");
    c.text2 = QColor("#5d6575");
    c.text3 = QColor("#8c93a3");
    c.hover = QColor("#e8e8ee");
    c.hoverSoft = QColor(15, 23, 42, 10);
    c.rowHover = QColor("#eef0f5");
    c.press = QColor("#dcdce4");
    c.tabActive = QColor("#e3edfc");
    c.tabActiveBorder = QColor("#b7cff5");
    c.tabActiveAccent = QColor("#007aff");
    c.tabActiveText = QColor("#0f172a");
    c.tabInactiveText = QColor("#636b7b");
    c.scroll = QColor("#c6c6ce");
    c.scrollHover = QColor("#9898a2");
    c.accent = QColor("#007aff");
    c.accentHover = QColor("#1a88ff");
    c.accentPress = QColor("#0062cc");
    c.accentText = QColor("#0066d6");
    c.accentSoft = QColor("#e3eefd");
    c.selectionBg = QColor("#e1edfd");
    c.selectionHover = QColor("#d3e5fc");
    c.selectionText = QColor("#111827");
    c.onAccent = QColor("#ffffff");
    c.green = QColor("#1f9d55");
    c.teal = QColor("#0b84b8");
    c.purple = QColor("#6d5bd0");
    c.orange = QColor("#c8760a");
    c.favorite = QColor("#d99a14");
    c.red = QColor("#d9383a");
    c.busy = QColor("#6359d4");
    c.busyText = QColor("#4a40b5");
    c.busySurface = QColor("#f4f3fc");
    c.connectedSurface = QColor("#eefaf3");
    c.separator = QColor(15, 23, 42, 18);
    c.separatorStrong = QColor(15, 23, 42, 28);
    c.barOff = QColor("#d6d6dc");
    return c;
}

QString resolvedUiFontFamily()
{
    static const QString family = [] {
        for (const char *name : {"SF Pro Text", "Inter", "Segoe UI"})
            if (QFontDatabase::hasFamily(QLatin1String(name)))
                return QString::fromLatin1(name);
        return QStringLiteral("Segoe UI");
    }();
    return family;
}

QString resolvedDisplayFontFamily()
{
    static const QString family = [] {
        for (const char *name : {"SF Pro Display", "Inter", "Segoe UI"})
            if (QFontDatabase::hasFamily(QLatin1String(name)))
                return QString::fromLatin1(name);
        return resolvedUiFontFamily();
    }();
    return family;
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
    kSurfaceAlt = c.surfaceAlt;
    kBusySurface = c.busySurface;
    kConnectedSurface = c.connectedSurface;
    kSeparator = c.separator;
    kSeparatorStrong = c.separatorStrong;
    kLabel = c.text;
    kLabelSecondary = c.text2;
    kLabelTertiary = c.text3;
    kBlue = c.accent;
    kBlueText = c.accentText;
    kGreen = c.green;
    kTeal = c.teal;
    kPurple = c.purple;
    kOrange = c.orange;
    kFavorite = c.favorite;
    kRed = c.red;
    kBusy = c.busy;
    kBusyText = c.busyText;
    kTabActive = c.tabActive;
    kTabActiveBorder = c.tabActiveBorder;
    kTabActiveAccent = c.tabActiveAccent;
    kBarOff = c.barOff;
    kPopupFill = c.solid;
    kPopupBorder = c.solidBorder;
    kAccentSoft = c.accentSoft;
    kOnAccent = c.onAccent;
}

void applyMode(QApplication &app)
{
    const bool dark = g_mode == Theme::Dark;
    const Scheme c = dark ? darkScheme() : lightScheme();
    setColours(c);

    QPalette p;
    p.setColor(QPalette::Window, c.bg);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.solid);
    p.setColor(QPalette::AlternateBase, dark ? QColor("#171c26") : QColor("#f4f6fa"));
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.solid);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::ToolTipBase, c.solid);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::Highlight, c.selectionBg);
    p.setColor(QPalette::HighlightedText, c.selectionText);
    p.setColor(QPalette::Link, c.accentText);
    p.setColor(QPalette::PlaceholderText, c.text2);
    p.setColor(QPalette::Disabled, QPalette::Text, c.text3);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.text3);
    app.setPalette(p);

    static QString cachedSheets[2];
    const int slot = dark ? 1 : 0;
    if (cachedSheets[slot].isEmpty()) {
        const QHash<QString, QString> tokens = {
            {QStringLiteral("fontFamily"), resolvedUiFontFamily()},
            {QStringLiteral("displayFontFamily"), resolvedDisplayFontFamily()},
            {QStringLiteral("bg"), css(c.bg)}, {QStringLiteral("topBar"), css(c.topBar)},
            {QStringLiteral("text"), css(c.text)}, {QStringLiteral("text2"), css(c.text2)}, {QStringLiteral("text3"), css(c.text3)},
            {QStringLiteral("surface"), css(c.surface)}, {QStringLiteral("surfaceAlt"), css(c.surfaceAlt)},
            {QStringLiteral("glassTop"), css(c.glassTop)}, {QStringLiteral("glassBottom"), css(c.glassBottom)},
            {QStringLiteral("sidebar"), css(c.sidebar)}, {QStringLiteral("button"), css(c.button)},
            {QStringLiteral("hover"), css(c.hover)}, {QStringLiteral("hoverSoft"), css(c.hoverSoft)},
            {QStringLiteral("rowHover"), css(c.rowHover)}, {QStringLiteral("press"), css(c.press)}, {QStringLiteral("field"), css(c.field)},
            {QStringLiteral("hairline"), css(c.hairline)}, {QStringLiteral("focus"), css(c.focus)}, {QStringLiteral("tabActive"), css(c.tabActive)},
            {QStringLiteral("tabActiveBorder"), css(c.tabActiveBorder)}, {QStringLiteral("tabActiveAccent"), css(c.tabActiveAccent)},
            {QStringLiteral("tabActiveText"), css(c.tabActiveText)}, {QStringLiteral("tabInactiveText"), css(c.tabInactiveText)},
            {QStringLiteral("scroll"), css(c.scroll)}, {QStringLiteral("scrollHover"), css(c.scrollHover)},
            {QStringLiteral("solid"), css(c.solid)}, {QStringLiteral("solidBorder"), css(c.solidBorder)},
            {QStringLiteral("accent"), css(c.accent)}, {QStringLiteral("accentHover"), css(c.accentHover)},
            {QStringLiteral("accentPress"), css(c.accentPress)}, {QStringLiteral("accentText"), css(c.accentText)},
            {QStringLiteral("accentSoft"), css(c.accentSoft)}, {QStringLiteral("selectionBg"), css(c.selectionBg)},
            {QStringLiteral("selectionHover"), css(c.selectionHover)}, {QStringLiteral("selectionText"), css(c.selectionText)},
            {QStringLiteral("onAccent"), css(c.onAccent)}, {QStringLiteral("orange"), css(c.orange)},
        };
        const QString tmpl = QString::fromUtf8(kStyleSheet);
        QString sheet;
        sheet.reserve(tmpl.size() + 512);
        qsizetype pos = 0;
        while (pos < tmpl.size()) {
            const qsizetype at = tmpl.indexOf(QLatin1Char('@'), pos);
            if (at < 0) {
                sheet.append(QStringView(tmpl).mid(pos));
                break;
            }
            const qsizetype end = tmpl.indexOf(QLatin1Char('@'), at + 1);
            if (end < 0) {
                sheet.append(QStringView(tmpl).mid(pos));
                break;
            }
            sheet.append(QStringView(tmpl).mid(pos, at - pos));
            const QString key = tmpl.mid(at + 1, end - at - 1);
            const auto it = tokens.constFind(key);
            if (it != tokens.cend()) {
                sheet.append(it.value());
                pos = end + 1;
            } else {
                sheet.append(QLatin1Char('@'));
                pos = at + 1;
            }
        }
        cachedSheets[slot] = std::move(sheet);
    }
    app.setStyleSheet(cachedSheets[slot]);
}

class DialogTitleBars : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show && watched->isWidgetType()) {
            auto *w = static_cast<QWidget *>(watched);
            if (w->isWindow() && qobject_cast<QDialog *>(w))
                Theme::darkTitleBar(w);
        }
        return false;
    }
};

} // namespace

namespace Theme {

void apply(QApplication &app)
{
    readMode();
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    // Native Windows hinting (ClearType / DirectWrite) keeps 12-14px UI text crisp instead of blurry.
    QFont base(resolvedUiFontFamily());
    base.setPixelSize(13);
    base.setHintingPreference(QFont::PreferVerticalHinting);
    base.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::PreferQuality));
    app.setFont(base);
    applyMode(app);
    app.installEventFilter(new DialogTitleBars(&app));

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
    for (QWidget *window : QApplication::topLevelWidgets()) {
        if (window->isVisible() && qobject_cast<QDialog *>(window))
            darkTitleBar(window);
    }
    emit notifier()->changed();
}

QString uiFontFamily()
{
    return resolvedUiFontFamily();
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
    f.setFamily(pixelSize >= 18 ? resolvedDisplayFontFamily() : resolvedUiFontFamily());
    f.setHintingPreference(QFont::PreferVerticalHinting);
    f.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::PreferQuality));
    f.setPixelSize(pixelSize);
    f.setWeight(weight);
    cache.insert(key, f);
    return f;
}

QFont monoFont(int pixelSize, QFont::Weight weight)
{
    static QHash<int, QFont> cache;
    const int key = pixelSize * 1000 + int(weight);
    const auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return *it;
    static const QString family = [] {
        for (const char *name : {"SF Mono", "JetBrains Mono", "Cascadia Mono", "Consolas", "Courier New"})
            if (QFontDatabase::hasFamily(QLatin1String(name)))
                return QString::fromLatin1(name);
        return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    }();
    QFont f(family);
    f.setHintingPreference(QFont::PreferVerticalHinting);
    f.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::PreferQuality));
    f.setPixelSize(pixelSize);
    f.setWeight(weight);
    cache.insert(key, f);
    return f;
}

void darkTitleBar(QWidget *window)
{
    const BOOL on = g_mode == Dark ? TRUE : FALSE;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof(on));
    const QColor background = window->palette().color(QPalette::Window);
    const QColor text = window->palette().color(QPalette::WindowText);
    const COLORREF caption = RGB(background.red(), background.green(), background.blue());
    const COLORREF foreground = RGB(text.red(), text.green(), text.blue());
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &foreground, sizeof(foreground));
}

} // namespace Theme
