#include "Autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace {

QString runKey() { return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"); }

// Dev runs (RCONNECT_TEST) get their own name, so they never replace the real entry.
QString valueName()
{
    return qEnvironmentVariableIsSet("RCONNECT_TEST") ? QStringLiteral("RConnect.Test") : QStringLiteral("RConnect");
}

QString command()
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QStringLiteral("\"%1\" --autostart").arg(exe);
}

} // namespace

namespace Autostart {

bool isEnabled()
{
    return QSettings(runKey(), QSettings::NativeFormat).contains(valueName());
}

void setEnabled(bool on)
{
    QSettings run(runKey(), QSettings::NativeFormat);
    if (on)
        run.setValue(valueName(), command());
    else
        run.remove(valueName());
}

void refresh()
{
    QSettings run(runKey(), QSettings::NativeFormat);
    if (run.contains(valueName()) && run.value(valueName()).toString() != command())
        run.setValue(valueName(), command());
}

} // namespace Autostart
