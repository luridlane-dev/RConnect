#include "Log.h"

#include <QAtomicInt>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

namespace {

// Past this size the log is moved to rconnect.log.old (replacing the previous one).
constexpr qint64 kMaxBytes = 2 * 1024 * 1024;

QAtomicInt g_detailed(-1); // -1: not read from the settings yet

} // namespace

QString Log::path()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/rconnect.log");
}

void Log::write(const QString &line)
{
    static QMutex mutex;
    QMutexLocker lock(&mutex);

    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    QFile f(path());
    if (f.size() > kMaxBytes) {
        QFile::remove(path() + QStringLiteral(".old"));
        f.rename(path() + QStringLiteral(".old"));
    }
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    QTextStream out(&f);
    out << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) << "  " << line << '\n';
}

bool Log::detailed()
{
    if (g_detailed.loadRelaxed() < 0)
        g_detailed.storeRelaxed(QSettings().value(QStringLiteral("log/detailed"), false).toBool() ? 1 : 0);
    return g_detailed.loadRelaxed() == 1;
}

void Log::setDetailed(bool on)
{
    g_detailed.storeRelaxed(on ? 1 : 0);
    QSettings().setValue(QStringLiteral("log/detailed"), on);
    write(on ? QStringLiteral("detailed logging turned on") : QStringLiteral("detailed logging turned off"));
}

void Log::detail(const QString &line)
{
    if (detailed())
        write(line);
}
