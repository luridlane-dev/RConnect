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
* { font-family: "Segoe UI"; font-size: 12px; }
QWidget { color: #e8e8e8; }
QMainWindow, QDialog, QWidget#root { background: #1c1c1c; }

QWidget#topBar { background: #141414; border-bottom: 1px solid #2e2e2e; }
QToolButton#menuButton {
    background: transparent; border: none; border-radius: 0; color: #b0b0b0; padding: 0; font-size: 16px;
}
QToolButton#menuButton:hover { background: #262626; color: #fff; }
QToolButton#winButton {
    background: transparent; border: none; border-radius: 0; color: #b0b0b0; padding: 0; font-size: 13px;
}
QToolButton#winButton:hover { background: #2c2c2c; color: #fff; }
QToolButton#winClose:hover { background: #c42b1c; color: #fff; }
QFrame#popup { background: #202020; border: 1px solid #3a3a3a; border-radius: 3px; }
QLabel#brandName { font-size: 18px; font-weight: 600; color: #fff; }
QLabel#panelFooter { color: #858585; font-size: 11px; padding: 2px 16px; }
QLabel#brandSummary { color: #8f8f8f; font-size: 12px; }
QLabel#popupTitle { font-size: 13px; font-weight: 600; color: #fff; }
QLabel#fieldHint { color: #858585; font-size: 11px; }
QToolButton#barButton {
    background: transparent; border: none; border-radius: 0; color: #b0b0b0; padding: 0 12px; font-size: 12px;
}
QToolButton#barButton:hover { background: #262626; color: #fff; }
QToolButton#settingsButton {
    font-family: "Segoe MDL2 Assets"; font-size: 15px; background: transparent; border: 1px solid transparent;
    border-radius: 3px; color: #b0b0b0; padding: 0;
}
QToolButton#settingsButton:hover { background: #2a2a2a; border-color: #3a3a3a; color: #fff; }
QToolButton#sectionToggle {
    background: transparent; border: none; color: #9fc9ee; padding: 2px 0; font-weight: 600;
}
QToolButton#sectionToggle:hover { color: #cfe6fa; }
QPlainTextEdit {
    background: #202020; border: 1px solid #404040; border-radius: 3px; padding: 3px 5px;
    selection-background-color: #0f6cbd;
}
QPlainTextEdit:focus { border-color: #3a96dd; }

QListView { background: transparent; border: none; outline: none; }
QScrollBar:vertical { background: transparent; width: 9px; margin: 0; }
QScrollBar::handle:vertical { background: #3e3e3e; border-radius: 3px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #505050; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QPushButton, QToolButton {
    background: #2d2d2d; border: 1px solid #404040; border-radius: 3px; padding: 5px 14px;
}
QPushButton:hover, QToolButton:hover { background: #363636; border-color: #505050; }
QPushButton:pressed, QToolButton:pressed { background: #262626; }
QPushButton#primaryButton { background: #0f6cbd; border-color: #0f6cbd; color: #fff; }
QPushButton#primaryButton:hover { background: #2886de; border-color: #2886de; }

QLineEdit, QSpinBox, QComboBox {
    background: #202020; border: 1px solid #404040; border-radius: 3px; padding: 5px 8px;
    selection-background-color: #0f6cbd;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #3a96dd; }
QComboBox QAbstractItemView { background: #202020; border: 1px solid #404040; selection-background-color: #363636; }
QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 14px; height: 14px; border-radius: 2px; border: 1px solid #5a5a5a; background: #202020; }
QCheckBox::indicator:checked { background: #0f6cbd; border-color: #0f6cbd; }

QTabBar { background: transparent; }
QTabBar::tab {
    background: transparent; color: #a0a0a0; padding: 0 10px; margin: 0; border: none;
    border-right: 1px solid #242424; min-width: 80px; max-width: 190px;
}
QTabBar::tab:hover { color: #e8e8e8; background: #1e1e1e; }
QTabBar::tab:selected { color: #fff; background: #2b2b2b; }
QToolButton#tabClose { background: transparent; border: none; border-radius: 2px; color: #9a9a9a; font-size: 14px; padding: 0; margin-right: 4px; }
QToolButton#tabClose:hover { background: #444444; color: #fff; }

QWidget#emptyPage { background: #1c1c1c; }

QWidget#sessionOverlay { background: #1c1c1c; }
QLabel#overlayTitle { font-size: 18px; font-weight: 600; color: #fff; }
QLabel#overlayMessage { color: #a8a8a8; font-size: 13px; }

QMenu { background: #252525; border: 1px solid #3a3a3a; padding: 3px; }
QMenu::item { padding: 5px 22px; border-radius: 2px; }
QMenu::item:selected { background: #343434; }
QMenu::separator { height: 1px; background: #3a3a3a; margin: 3px 6px; }
QToolTip { background: #2d2d2d; color: #e8e8e8; border: 1px solid #404040; padding: 3px 5px; }
)";

} // namespace

namespace Theme {

void apply(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette p;
    const QColor bg(0x1c, 0x1c, 0x1c), field(0x20, 0x20, 0x20), text(0xe8, 0xe8, 0xe8);
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, field);
    p.setColor(QPalette::AlternateBase, bg);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(0x2d, 0x2d, 0x2d));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::ToolTipBase, QColor(0x2d, 0x2d, 0x2d));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Highlight, QColor(0x0f, 0x6c, 0xbd));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(0x80, 0x80, 0x80));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x70, 0x70, 0x70));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x70, 0x70, 0x70));
    app.setPalette(p);
    app.setStyleSheet(QString::fromUtf8(kStyleSheet));

    app.setEffectEnabled(Qt::UI_AnimateCombo, false);
    app.setEffectEnabled(Qt::UI_FadeMenu, false);
    app.setEffectEnabled(Qt::UI_FadeTooltip, false);
}

void darkTitleBar(QWidget *window)
{
    const BOOL on = TRUE;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &on, sizeof(on));
}

} // namespace Theme
