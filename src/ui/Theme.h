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
inline QColor kBackground(0x06, 0x06, 0x07);
inline QColor kCard(0x10, 0x10, 0x12);
inline QColor kSurfaceAlt(0x0b, 0x0b, 0x0d);
inline QColor kBusySurface(0x15, 0x14, 0x1f);
inline QColor kConnectedSurface(0x0e, 0x1a, 0x13);
inline QColor kSeparator(255, 255, 255, 16);
inline QColor kSeparatorStrong(255, 255, 255, 26);
inline QColor kLabel(0xf4, 0xf4, 0xf6);
inline QColor kLabelSecondary(0x98, 0x98, 0xa0);
inline QColor kLabelTertiary(0x63, 0x63, 0x6c);
inline QColor kBlue(0x0a, 0x84, 0xff);
inline QColor kBlueText(0x52, 0xa5, 0xff);
inline QColor kGreen(0x34, 0xc7, 0x7b);
inline QColor kTeal(0x4f, 0xb8, 0xe8);
inline QColor kPurple(0x96, 0x83, 0xf5);
inline QColor kOrange(0xf0, 0xa3, 0x32);
inline QColor kFavorite(0xf5, 0xc8, 0x42);
inline QColor kRed(0xef, 0x5b, 0x5e);
inline QColor kBusy(0x8b, 0x7c, 0xf6);
inline QColor kBusyText(0xc4, 0xbc, 0xff);
inline QColor kTabActive(0x18, 0x18, 0x1b);
inline QColor kTabActiveBorder(0x28, 0x28, 0x2d);
inline QColor kTabActiveAccent(0x0a, 0x84, 0xff);
inline QColor kBarOff(0x26, 0x26, 0x2b);
inline QColor kPopupFill(0x11, 0x11, 0x14);
inline QColor kPopupBorder(255, 255, 255, 24);
inline QColor kAccentSoft(0x13, 0x22, 0x36);
inline QColor kOnAccent(0xff, 0xff, 0xff);

// Crisp UI font (SF Pro / Inter when installed, Segoe UI otherwise).
QFont font(int pixelSize, QFont::Weight weight = QFont::Normal);
QFont monoFont(int pixelSize, QFont::Weight weight = QFont::Normal);
QString uiFontFamily();
}
