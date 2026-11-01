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

// Colours; they change with the mode. Painted surfaces are translucent so the window's soft
// gradient shows through them, like frosted glass.
inline QColor kBackground(0x00, 0x00, 0x00);
inline QColor kCard(255, 255, 255, 15);
inline QColor kCardHover(255, 255, 255, 27);
inline QColor kCardPressed(255, 255, 255, 20);
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
inline QColor kTabPill(255, 255, 255, 28);
inline QColor kBarOff(255, 255, 255, 46);
inline QColor kPopupFill(0x14, 0x14, 0x17);
inline QColor kPopupBorder(0x2a, 0x2a, 0x2f);
inline QColor kIconLine(0xd1, 0xd1, 0xd6);

// Segoe UI Variable on Windows 11, Segoe UI before.
QFont font(int pixelSize, QFont::Weight weight = QFont::Normal);
}
