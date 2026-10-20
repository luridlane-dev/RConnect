#pragma once

class QApplication;
class QWidget;

namespace Theme {
void apply(QApplication &app);
// Dark native title bar (Windows 10 1809+ / 11).
void darkTitleBar(QWidget *window);
}
