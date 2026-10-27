#include "Theme.h"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>
#include <QWidget>

#include <windows.h>
#include <dwmapi.h>

namespace {

const char *const kStyleSheet = R"(
* { font-family: "Segoe UI Variable Text", "Segoe UI"; font-size: 13px; }
QWidget { color: #ffffff; }
QMainWindow, QDialog, QWidget#root { background: #000000; }

QWidget#topBar { background: #0c0c0e; border-bottom: 1px solid #2c2c2e; }
QToolButton#menuButton {
    background: transparent; border: none; border-radius: 0; color: #98989f; padding: 0; font-size: 18px;
}
QToolButton#menuButton:hover { background: #1c1c1e; color: #ffffff; }
QToolButton#winButton {
    background: transparent; border: none; border-radius: 0; color: #98989f; padding: 0; font-size: 13px;
}
QToolButton#winButton:hover { background: #2c2c2e; color: #ffffff; }
QToolButton#winClose:hover { background: #ff453a; color: #ffffff; }
QFrame#popup { background: #000000; border: 1px solid #38383a; border-radius: 16px; }
QLabel#brandName { font-size: 32px; font-weight: 700; color: #ffffff; }
QLabel#brandSummary { color: #98989f; font-size: 13px; }
QLabel#emptyState { color: #636366; font-size: 14px; }
QLabel#popupTitle { font-size: 13px; font-weight: 600; color: #ffffff; }
QLabel#fieldHint { color: #98989f; font-size: 12px; }
QToolButton#barButton {
    background: transparent; border: none; border-radius: 0; color: #98989f; padding: 0 12px; font-size: 13px;
}
QToolButton#barButton:hover { background: #1c1c1e; color: #ffffff; }
QToolButton#settingsButton {
    font-family: "Segoe Fluent Icons", "Segoe MDL2 Assets"; font-size: 16px; background: #1c1c1e;
    border: none; border-radius: 18px; color: #98989f; padding: 0;
}
QToolButton#settingsButton:hover { background: #2c2c2e; color: #ffffff; }
QToolButton#sectionToggle {
    background: transparent; border: none; color: #0a84ff; padding: 2px 0; font-weight: 600;
}
QToolButton#sectionToggle:hover { color: #64acff; }
QPlainTextEdit {
    background: #1c1c1e; border: 1px solid #38383a; border-radius: 10px; padding: 5px 8px;
    selection-background-color: #0a84ff;
}
QPlainTextEdit:focus { border-color: #0a84ff; }

QListView { background: transparent; border: none; outline: none; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #3a3a3c; border-radius: 3px; min-height: 36px; }
QScrollBar::handle:vertical:hover { background: #636366; }
QScrollBar::handle:vertical:disabled { background: transparent; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QPushButton, QToolButton {
    background: #2c2c2e; border: none; border-radius: 10px; padding: 7px 16px;
}
QPushButton:hover, QToolButton:hover { background: #3a3a3c; }
QPushButton:pressed, QToolButton:pressed { background: #232325; }
QPushButton#primaryButton { background: #0a84ff; color: #ffffff; font-weight: 600; }
QPushButton#primaryButton:hover { background: #3d9bff; }
QPushButton#primaryButton:pressed { background: #0a6fd9; }
QPushButton#addButton {
    background: #0a84ff; color: #ffffff; font-weight: 600; border-radius: 18px; padding: 0 18px; min-height: 36px;
}
QPushButton#addButton:hover { background: #3d9bff; }
QPushButton#addButton:pressed { background: #0a6fd9; }

QFrame#segmented { background: #1c1c1e; border-radius: 10px; }
QPushButton#segment {
    background: transparent; color: #98989f; border: none; border-radius: 8px; padding: 0 14px;
    min-height: 28px; font-weight: 500;
}
QPushButton#segment:hover { color: #ffffff; background: transparent; }
QPushButton#segment:checked { background: #636366; color: #ffffff; }

QLineEdit, QSpinBox, QComboBox {
    background: #1c1c1e; border: 1px solid transparent; border-radius: 10px; padding: 7px 10px;
    selection-background-color: #0a84ff;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #0a84ff; }
QLineEdit#searchField { border-radius: 18px; padding: 0 12px; min-height: 36px; }
QComboBox QAbstractItemView {
    background: #1c1c1e; border: 1px solid #38383a; border-radius: 8px; selection-background-color: #2c2c2e; outline: none;
}
QCheckBox { spacing: 10px; }
QCheckBox::indicator { width: 18px; height: 18px; border-radius: 9px; border: 1.5px solid #636366; background: transparent; }
QCheckBox::indicator:checked { background: #30d158; border-color: #30d158; }

QTabBar { background: transparent; }
QTabBar::tab {
    background: transparent; color: #98989f; padding: 0 10px; margin: 0; border: none;
    min-width: 90px; max-width: 200px;
}
QTabBar::tab:hover { color: #ffffff; }
QTabBar::tab:selected { color: #ffffff; background: transparent; }
QToolButton#tabClose {
    background: transparent; border: none; border-radius: 9px; color: #98989f; font-size: 15px; padding: 0; margin-right: 6px;
}
QToolButton#tabClose:hover { background: #48484a; color: #ffffff; }

QWidget#emptyPage { background: #000000; }

QWidget#sessionOverlay { background: #000000; }
QLabel#overlayTitle { font-size: 22px; font-weight: 700; color: #ffffff; }
QLabel#overlayMessage { color: #98989f; font-size: 14px; }

QMenu { background: #1c1c1e; border: 1px solid #38383a; border-radius: 12px; padding: 5px; }
QMenu::item { padding: 7px 24px; border-radius: 7px; }
QMenu::item:selected { background: #0a84ff; }
QMenu::separator { height: 1px; background: #38383a; margin: 4px 8px; }
QToolTip { background: #2c2c2e; color: #ffffff; border: 1px solid #48484a; border-radius: 6px; padding: 5px 8px; }
QMessageBox { background: #1c1c1e; }
)";

} // namespace

namespace Theme {

void apply(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette p;
    const QColor bg(0x00, 0x00, 0x00), field(0x1c, 0x1c, 0x1e), text(0xff, 0xff, 0xff);
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, field);
    p.setColor(QPalette::AlternateBase, bg);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(0x2c, 0x2c, 0x2e));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::ToolTipBase, QColor(0x2c, 0x2c, 0x2e));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Highlight, QColor(0x0a, 0x84, 0xff));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(0x8e, 0x8e, 0x93));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x70, 0x70, 0x70));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x70, 0x70, 0x70));
    app.setPalette(p);
    app.setStyleSheet(QString::fromUtf8(kStyleSheet));

    app.setEffectEnabled(Qt::UI_AnimateCombo, false);
    app.setEffectEnabled(Qt::UI_FadeMenu, false);
    app.setEffectEnabled(Qt::UI_FadeTooltip, false);
}

QFont font(int pixelSize, QFont::Weight weight)
{
    QFont f;
    f.setFamilies({QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI")});
    f.setPixelSize(pixelSize);
    f.setWeight(weight);
    return f;
}

void darkTitleBar(QWidget *window)
{
    const BOOL on = TRUE;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &on, sizeof(on));
}

} // namespace Theme
