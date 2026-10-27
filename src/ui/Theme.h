#pragma once
#include <QColor>
#include <QFont>

class QApplication;
class QWidget;

namespace Theme {
void apply(QApplication &app);
// Dark native title bar (Windows 10 1809+ / 11).
void darkTitleBar(QWidget *window);

// Colours (iOS dark system colours).
inline const QColor kBackground(0x00, 0x00, 0x00);
inline const QColor kCard(0x1c, 0x1c, 0x1e);
inline const QColor kCardHover(0x2c, 0x2c, 0x2e);
inline const QColor kCardPressed(0x24, 0x24, 0x26);
inline const QColor kSeparator(0x38, 0x38, 0x3a);
inline const QColor kLabel(0xff, 0xff, 0xff);
inline const QColor kLabelSecondary(0x98, 0x98, 0x9f);
inline const QColor kLabelTertiary(0x63, 0x63, 0x66);
inline const QColor kBlue(0x0a, 0x84, 0xff);

// Segoe UI Variable on Windows 11, Segoe UI before.
QFont font(int pixelSize, QFont::Weight weight = QFont::Normal);
}
