#pragma once
#include <QString>
#include <QStringList>

// UI language. Translations are embedded .qm files.
namespace Language {
void init();                       // saved choice or the Windows language
QString current();
QStringList available();
QString nativeName(const QString &code);
void set(const QString &code);
}
