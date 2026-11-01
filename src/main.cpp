#include "core/Autostart.h"
#include "core/Language.h"
#include "core/UsageDiag.h"
#include "model/ServerStore.h"
#include "ui/Theme.h"
#include "ui/Workspace.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QIcon>
#include <QSettings>
#include <QStandardPaths>

#include <winsock2.h>
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

// `RConnect --usage-diag <file>`: writes every server's session state to <file> and exits (read only).
int usageDiagnostics(int argc, char *argv[], const QString &outFile)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("RConnect"));
    QCoreApplication::setApplicationName(QStringLiteral("RConnect"));
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    ServerStore store;
    store.load();
    QFile out(outFile);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 2;
    for (const ServerProfile &p : store.all())
        if (p.checkSessions && !p.user.isEmpty() && !p.password.isEmpty())
            out.write(UsageDiag::report(p).toUtf8());
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    for (int i = 1; i + 1 < argc; ++i)
        if (qstrcmp(argv[i], "--usage-diag") == 0)
            return usageDiagnostics(argc, argv, QString::fromLocal8Bit(argv[i + 1]));

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
    Autostart::refresh();

    const bool autostarted = QCoreApplication::arguments().contains(QStringLiteral("--autostart"));
    Workspace workspace(windowMarker);
    workspace.start(autostarted);
    const int rc = app.exec();

    if (instance)
        CloseHandle(instance);
    return rc;
}
