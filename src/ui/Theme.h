#pragma once
#include <QColor>
#include <QFont>
#include <QObject>

class QApplication;
class QWidget;

// Emits `changed` after the look was switched, so hand-painted widgets and cached icons can refresh.
class ThemeNotifier : public QObject {
    Q_OBJECT
signals:
    void changed();
};

namespace Theme {

enum Mode { Dark, Light };

// Reads the saved mode, then applies the palette and style sheet.
void apply(QApplication &app);
Mode mode();
// Switches the look of the whole program and remembers it.
void setMode(Mode mode);
ThemeNotifier *notifier();

// Native title bar in the colour of the current mode (Windows 10 1809+ / 11).
void darkTitleBar(QWidget *window);

// Colours for hand-painted widgets; they change with the mode (see Scheme in Theme.cpp).
inline QColor kBackground(0x00, 0x00, 0x00);
inline QColor kCard(0x0c, 0x0c, 0x0c);
inline QColor kCardHover(0x16, 0x16, 0x16);
inline QColor kCardPressed(0x10, 0x10, 0x10);
inline QColor kSeparator(255, 255, 255, 26);
inline QColor kSeparatorStrong(255, 255, 255, 56);
inline QColor kLabel(0xf5, 0xf5, 0xf7);
inline QColor kLabelSecondary(0xa1, 0xa1, 0xa6);
inline QColor kLabelTertiary(0x6e, 0x6e, 0x73);
inline QColor kBlue(0x0a, 0x84, 0xff);
inline QColor kBlueText(0x64, 0xa8, 0xff);
inline QColor kGreen(0x30, 0xd1, 0x58);
inline QColor kOrange(0xff, 0x9f, 0x0a);
inline QColor kRed(0xff, 0x45, 0x3a);
inline QColor kBusy(0xff, 0x5a, 0x4d); // a server somebody is using: stands out from every other state
inline QColor kBusyText(0xff, 0x8a, 0x80); // the name of the person on it
inline QColor kTabPill(255, 255, 255, 28);
inline QColor kTabActive(0x22, 0x22, 0x22); // the tab in front, and the start page button while it is shown
inline QColor kBarOff(255, 255, 255, 46);
inline QColor kPopupFill(0x14, 0x14, 0x17);
inline QColor kPopupBorder(0x2a, 0x2a, 0x2f);
inline QColor kIconLine(0xd1, 0xd1, 0xd6);
inline QColor kIconTile(0x4f, 0x5d, 0x6b); // the square behind a server's monitor icon
inline QColor kHeaderFill(0x2e, 0x33, 0x3b); // the bar that opens a group of servers

// Segoe UI, drawn without hinting.
QFont font(int pixelSize, QFont::Weight weight = QFont::Normal);
}
