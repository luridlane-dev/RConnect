#include "Log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>

namespace {

// Past this size the old log is replaced.
constexpr qint64 kMaxBytes = 1024 * 1024;

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
    if (f.size() > kMaxBytes)
        f.remove();
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    QTextStream out(&f);
    out << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) << "  " << line << '\n';
}
