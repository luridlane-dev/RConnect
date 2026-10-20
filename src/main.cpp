#include "core/Language.h"
#include "ui/Theme.h"
#include "ui/Workspace.h"

#include <QApplication>
#include <QIcon>
#include <QSettings>
#include <QStandardPaths>

#include <windows.h>

namespace {

// Raise the running instance instead of starting a second one.
void activateRunningInstance(const wchar_t *marker)
{
    struct Ctx { const wchar_t *marker; HWND found; } ctx{marker, nullptr};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        if (GetPropW(hwnd, c->marker)) {
            c->found = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.found)
        return;
    if (IsIconic(ctx.found))
        ShowWindow(ctx.found, SW_RESTORE);
    SetForegroundWindow(ctx.found);
}

} // namespace

int main(int argc, char *argv[])
{
    // Development runs get their own config and instance.
    const bool testMode = qEnvironmentVariableIsSet("RCONNECT_TEST");
    const wchar_t *mutexName = testMode ? L"Local\\RConnect.Test.Instance" : L"Local\\RConnect.Instance";
    const wchar_t *windowMarker = testMode ? L"RConnect.Test.MainWindow" : L"RConnect.MainWindow";

    HANDLE instance = CreateMutexW(nullptr, TRUE, mutexName);
    const DWORD mutexError = GetLastError();
    // ACCESS_DENIED: the running instance is elevated.
    if (mutexError == ERROR_ALREADY_EXISTS || mutexError == ERROR_ACCESS_DENIED) {
        activateRunningInstance(windowMarker);
        if (instance)
            CloseHandle(instance);
        return 0;
    }

    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("RConnect"));
    QApplication::setApplicationName(QStringLiteral("RConnect"));
    QApplication::setApplicationVersion(QStringLiteral(RCONNECT_VERSION));
    if (testMode) {
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
    }

    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icon.png")));
    Theme::apply(app);
    Language::init();

    Workspace workspace(windowMarker);
    workspace.start();
    const int rc = app.exec();

    if (instance)
        CloseHandle(instance);
    return rc;
}
