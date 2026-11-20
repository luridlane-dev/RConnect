#include "Theme.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
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
QWidget#root, QMainWindow, QDialog, QMessageBox { background: @bg@; }

QWidget#topBar { background: @topBar@; border-bottom: 1px solid @border@; }
QToolButton#menuButton, QToolButton#homeTab, QToolButton#barButton {
    background: transparent; border: none; border-radius: 3px; color: @text2@; margin: 5px 3px; padding: 0;
}
QToolButton#menuButton { font-size: 19px; padding-bottom: 2px; }
QToolButton#barButton { padding: 0 12px; }
QToolButton#menuButton:hover, QToolButton#homeTab:hover, QToolButton#barButton:hover {
    background: @hover@; color: @text@;
}
QToolButton#homeTab:checked { background: @accentSoft@; }
QToolButton#winButton, QToolButton#winClose {
    background: transparent; border: none; border-radius: 0; color: @text2@; padding: 0; font-size: 14px;
}
QToolButton#winButton:hover { background: @hover@; color: @text@; }
QToolButton#winClose:hover { background: #c42b1c; color: #ffffff; }
QToolButton#trafficClose, QToolButton#trafficMin, QToolButton#trafficMax {
    border: none; border-radius: 6px; padding: 0; min-width: 12px; max-width: 12px; min-height: 12px; max-height: 12px;
}
QToolButton#trafficClose { background: #ff5f57; }
QToolButton#trafficMin { background: #febc2e; }
QToolButton#trafficMax { background: #28c840; }
QToolButton#trafficClose:hover { background: #ff8780; }
QToolButton#trafficMin:hover { background: #ffd060; }
QToolButton#trafficMax:hover { background: #5bdc70; }

QLabel#brandName { font-size: 18px; font-weight: 600; }
QLabel#pageTitle { font-size: 28px; font-weight: 600; }
QLabel#pageSubtitle, QLabel#fieldHint { color: @text2@; }
QLabel#fieldHint { font-size: 12px; }
QLabel#settingsHeading, QLabel#popupTitle { font-size: 14px; font-weight: 600; }
QLabel#settingsTitle { font-size: 24px; font-weight: 600; }
QFrame#settingsRail { background: @sidebar@; border-right: 1px solid @border@; }
QFrame#settingsFooter { background: @sidebar@; border-top: 1px solid @border@; }
QFrame#settingsSection { background: @surface@; border: none; border-radius: 3px; }
QListWidget#settingsNavigation { background: transparent; border: none; outline: none; }
QListWidget#settingsNavigation::item {
    min-height: 19px; color: @text2@; margin: 1px 0; padding: 8px 8px;
    border-left: 2px solid transparent; border-radius: 2px;
}
QListWidget#settingsNavigation::item:hover { background: @hoverSoft@; color: @text@; }
QListWidget#settingsNavigation::item:selected {
    background: @accentSoft@; color: @accentText@; border-left-color: @focus@;
}
QLabel#emptyState { color: @text2@; font-size: 14px; }
QLabel#warningText { color: @orange@; }

QFrame#sidebar { background: @sidebar@; border-right: 1px solid @border@; }
QFrame#detailCard, QFrame#popup { background: @surface@; border: 1px solid @border@; border-radius: 3px; }
QFrame#navPill { background: @accentSoft@; border-radius: 3px; }
QPushButton#navButton {
    background: transparent; border: 1px solid transparent; border-radius: 3px; color: @text2@;
    text-align: left; min-height: 24px; padding: 7px 11px; font-weight: 600;
}
QPushButton#navButton:hover { background: @hoverSoft@; color: @text@; }
QPushButton#navButton:pressed { background: @hoverSoft@; }
QPushButton#navButton:checked { background: transparent; color: @accentText@; }

QPlainTextEdit, QLineEdit, QSpinBox, QComboBox {
    background: @field@; border: 1px solid @border@; border-radius: 3px; padding: 6px 9px;
    selection-background-color: @accent@; selection-color: @onAccent@;
}
QPlainTextEdit:focus, QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: @focus@; }
QLineEdit#searchField { padding: 0 9px; min-height: 38px; }
QComboBox QAbstractItemView {
    background: @solid@; border: 1px solid @solidBorder@; selection-background-color: @accent@;
    selection-color: @onAccent@; outline: none;
}
QCheckBox { spacing: 8px; }
QCheckBox::indicator:unchecked { background: @field@; border: 1px solid @focus@; border-radius: 2px; }
QCheckBox::indicator:unchecked:hover { background: @hover@; }

QListView { background: transparent; border: none; outline: none; }
QTableView, QTreeView {
    background: @surface@; alternate-background-color: @surfaceAlt@; border: 1px solid @border@; border-radius: 3px;
    outline: none; selection-background-color: @accent@; selection-color: @onAccent@;
}
QTableView { gridline-color: transparent; }
QTableView::item { padding: 4px 9px; border: none; }
QTableView::item:selected, QTreeView::item:selected { color: @onAccent@; }
QHeaderView, QTableCornerButton::section { background: @surfaceAlt@; border: none; }
QHeaderView::section {
    background: @surfaceAlt@; color: @text2@; border: none; border-bottom: 1px solid @border@;
    padding: 8px 9px; font-weight: 600;
}
QAbstractScrollArea::corner { background: @surface@; }
QScrollBar:horizontal { height: 8px; margin: 2px; background: transparent; }
QScrollBar:vertical { width: 8px; margin: 2px; background: transparent; }
QScrollBar::handle { background: @scroll@; border-radius: 4px; }
QScrollBar::handle:horizontal { min-width: 32px; }
QScrollBar::handle:vertical { min-height: 32px; }
QScrollBar::handle:hover { background: @scrollHover@; }
QScrollBar::handle:disabled { background: transparent; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QPushButton, QToolButton {
    background: @button@; border: 1px solid @border@; border-radius: 3px; padding: 6px 12px;
}
QPushButton:hover, QToolButton:hover { background: @hover@; }
QPushButton:pressed, QToolButton:pressed { background: @press@; }
QPushButton:focus, QToolButton:focus, QPushButton#navButton:focus, QPushButton#segment:focus {
    border-color: @focus@;
}
QPushButton#primaryButton, QPushButton#addButton {
    background: @accent@; border-color: transparent; color: @onAccent@; font-weight: 600;
}
QPushButton#primaryButton:hover, QPushButton#addButton:hover { background: @accentHover@; }
QPushButton#primaryButton:pressed, QPushButton#addButton:pressed { background: @accentPress@; }
QPushButton#primaryButton:focus, QPushButton#addButton:focus { border-color: @onAccent@; }
QPushButton:disabled, QToolButton:disabled, QPushButton#primaryButton:disabled, QPushButton#addButton:disabled {
    background: @surfaceAlt@; color: @text3@; border-color: @border@;
}
QPushButton#addButton { padding: 0 16px; min-height: 38px; }
QToolButton#sectionToggle {
    background: transparent; border-color: transparent; color: @accentText@; padding: 3px 4px; font-weight: 600;
}
QToolButton#sectionToggle:hover { background: @hoverSoft@; }
QToolButton#sectionToggle:focus { border-color: @focus@; }

QFrame#segmented { background: @surfaceAlt@; border: 1px solid @border@; border-radius: 4px; }
QPushButton#segment {
    background: transparent; color: @text2@; border: 1px solid transparent; border-radius: 3px;
    padding: 0 9px; min-height: 26px; font-weight: 600;
}
QPushButton#segment:hover { color: @text@; background: @hoverSoft@; }
QPushButton#segment:pressed { background: @hover@; color: @text@; }
QPushButton#segment:checked { background: @accentSoft@; color: @accentText@; }
QPushButton#segment:checked:pressed { background: @press@; }
QPushButton#segment[iconOnly="true"] { padding: 0; }
QPushButton#segment[iconOnly="true"]:focus { border-color: @focus@; }

QTabBar { background: transparent; }
QTabBar::tab {
    background: transparent; color: @text2@; padding: 0 4px 0 12px; margin: 5px 2px;
    border: none; border-radius: 3px; font-size: 12px; min-width: 80px; max-width: 180px;
}
QTabBar::tab:hover { color: @text@; background: @hoverSoft@; }
QTabBar::tab:selected { background: transparent; color: @text@; font-weight: 600; }
QToolButton#tabClose {
    background: transparent; border: none; border-radius: 4px; color: @text2@; font-size: 13px; padding: 0;
}
QToolButton#tabClose:hover { background: @hover@; color: @text@; }

QWidget#emptyPage, QScrollArea#settingsScroll, QWidget#settingsContent { background: transparent; border: none; }
QWidget#sessionOverlay { background: @bg@; }
QLabel#overlayTitle { font-size: 22px; font-weight: 600; }
QLabel#overlayMessage { color: @text2@; font-size: 14px; }

QMenu { background: @solid@; border: 1px solid @solidBorder@; border-radius: 3px; padding: 5px; }
QMenu::item { padding: 7px 22px; border-radius: 2px; }
QMenu::item:selected { background: @accentSoft@; color: @accentText@; }
QMenu::item:disabled { color: @text3@; }
QMenu::separator { height: 1px; background: @border@; margin: 4px 8px; }
QToolTip { background: @solid@; color: @text@; border: 1px solid @solidBorder@; padding: 5px 9px; font-size: 12px; }
)";

struct Scheme {
    QColor bg, topBar, surface, surfaceAlt, sidebar, button, field, border, focus, solid, solidBorder;
    QColor text, text2, text3;
    QColor hover, hoverSoft, press, tabActive, scroll, scrollHover;
    QColor accent, accentHover, accentPress, accentText, accentSoft, onAccent;
    QColor green, orange, favorite, red, busy, busyText;
    QColor busySurface, connectedSurface, separator, separatorStrong, barOff;
};

Scheme darkScheme()
{
    Scheme c;
    c.bg = QColor("#080808");
    c.topBar = QColor("#101010");
    c.surface = QColor("#151515");
    c.surfaceAlt = QColor("#111111");
    c.sidebar = QColor("#101010");
    c.button = QColor("#212121");
    c.field = QColor("#141414");
    c.border = QColor("#262626");
    c.focus = QColor("#777777");
    c.solid = QColor("#1c1c1c");
    c.solidBorder = QColor("#333333");
    c.text = QColor("#f5f5f5");
    c.text2 = QColor("#c2c2c2");
    c.text3 = QColor("#9d9d9d");
    c.hover = QColor("#2a2a2a");
    c.hoverSoft = QColor(255, 255, 255, 19);
    c.press = QColor("#353535");
    c.tabActive = QColor("#2b2b2b");
    c.scroll = QColor("#484848");
    c.scrollHover = QColor("#747474");
    c.accent = QColor("#ededed");
    c.accentHover = QColor("#ffffff");
    c.accentPress = QColor("#d1d1d1");
    c.accentText = QColor("#eeeeee");
    c.accentSoft = QColor("#282828");
    c.onAccent = QColor("#141414");
    c.green = QColor("#65dbab");
    c.orange = QColor("#ffdf42");
    c.favorite = QColor("#ffe34d");
    c.red = QColor("#fb8393");
    c.busy = QColor("#fb8393");
    c.busyText = QColor("#ffa6b2");
    c.busySurface = QColor("#291b20");
    c.connectedSurface = QColor("#172820");
    c.separator = QColor("#222222");
    c.separatorStrong = c.border;
    c.barOff = QColor("#484848");
    return c;
}

Scheme lightScheme()
{
    Scheme c;
    c.bg = QColor("#f4f6fc");
    c.topBar = QColor("#ffffff");
    c.surface = QColor("#ffffff");
    c.surfaceAlt = QColor("#f0f3fa");
    c.sidebar = QColor("#eef1f9");
    c.button = QColor("#ffffff");
    c.field = QColor("#ffffff");
    c.border = QColor("#e1e6f0");
    c.focus = QColor("#6878c4");
    c.solid = QColor("#ffffff");
    c.solidBorder = QColor("#c5d0e3");
    c.text = QColor("#19233a");
    c.text2 = QColor("#4c5974");
    c.text3 = QColor("#606e88");
    c.hover = QColor("#e9edf8");
    c.hoverSoft = QColor(70, 86, 146, 17);
    c.press = QColor("#dce4f6");
    c.tabActive = QColor("#e3e9ff");
    c.scroll = QColor("#bcc7db");
    c.scrollHover = QColor("#8594b0");
    c.accent = QColor("#414eb3");
    c.accentHover = QColor("#3542a3");
    c.accentPress = QColor("#2c3688");
    c.accentText = QColor("#3746a3");
    c.accentSoft = QColor("#e3e9ff");
    c.onAccent = QColor("#ffffff");
    c.green = QColor("#087554");
    c.orange = QColor("#795800");
    c.favorite = QColor("#e5b500");
    c.red = QColor("#be3a54");
    c.busy = QColor("#be3a54");
    c.busyText = QColor("#a62a43");
    c.busySurface = QColor("#fbeef0");
    c.connectedSurface = QColor("#e5f3eb");
    c.separator = QColor("#e1e7f2");
    c.separatorStrong = c.border;
    c.barOff = QColor("#d7dfee");
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
    kOrange = c.orange;
    kFavorite = c.favorite;
    kRed = c.red;
    kBusy = c.busy;
    kBusyText = c.busyText;
    kTabActive = c.tabActive;
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
    p.setColor(QPalette::Base, c.field);
    p.setColor(QPalette::AlternateBase, c.surfaceAlt);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.button);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::ToolTipBase, c.solid);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::Highlight, c.accent);
    p.setColor(QPalette::HighlightedText, c.onAccent);
    p.setColor(QPalette::Link, c.accentText);
    p.setColor(QPalette::PlaceholderText, c.text2);
    p.setColor(QPalette::Disabled, QPalette::Text, c.text3);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.text3);
    app.setPalette(p);

    const QHash<QString, QColor> tokens = {
        {"bg", c.bg}, {"topBar", c.topBar}, {"text", c.text}, {"text2", c.text2}, {"text3", c.text3},
        {"surface", c.surface}, {"surfaceAlt", c.surfaceAlt}, {"sidebar", c.sidebar}, {"button", c.button},
        {"hover", c.hover}, {"hoverSoft", c.hoverSoft}, {"press", c.press}, {"field", c.field}, {"border", c.border},
        {"focus", c.focus},
        {"scroll", c.scroll},
        {"scrollHover", c.scrollHover}, {"solid", c.solid}, {"solidBorder", c.solidBorder}, {"accent", c.accent},
        {"accentHover", c.accentHover}, {"accentPress", c.accentPress}, {"accentText", c.accentText},
        {"accentSoft", c.accentSoft}, {"onAccent", c.onAccent}, {"orange", c.orange}
    };
    QString sheet = QString::fromUtf8(kStyleSheet);
    for (auto it = tokens.cbegin(); it != tokens.cend(); ++it)
        sheet.replace(QLatin1Char('@') + it.key() + QLatin1Char('@'), css(it.value()));
    app.setStyleSheet(sheet);
}

class DialogTitleBars : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show && qobject_cast<QDialog *>(watched))
            Theme::darkTitleBar(static_cast<QWidget *>(watched));
        return false;
    }
};

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
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof(on));
    const QColor background = window->palette().color(QPalette::Window);
    const QColor text = window->palette().color(QPalette::WindowText);
    const COLORREF caption = RGB(background.red(), background.green(), background.blue());
    const COLORREF foreground = RGB(text.red(), text.green(), text.blue());
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &foreground, sizeof(foreground));
}

} // namespace Theme
