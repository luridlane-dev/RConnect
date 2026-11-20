#include "LocalCache.h"

#include "UpdateChecker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVersionNumber>

#include <string>
#include <windows.h>

namespace {

// Check the path of the opened file so a directory link or replacement cannot redirect deletion.
bool isExpectedPath(HANDLE handle, const QString &expected)
{
    const DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (needed == 0)
        return false;
    std::wstring buffer(needed, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), needed, FILE_NAME_NORMALIZED);
    if (length == 0 || length >= needed)
        return false;
    QString resolved = QDir::fromNativeSeparators(QString::fromWCharArray(buffer.data(), length));
    if (resolved.startsWith(QLatin1String("//?/UNC/"), Qt::CaseInsensitive))
        resolved = QStringLiteral("//") + resolved.mid(8);
    else if (resolved.startsWith(QLatin1String("//?/")))
        resolved.remove(0, 4);
    return resolved.compare(expected, Qt::CaseInsensitive) == 0;
}

bool removeUnusedFile(const QString &path)
{
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    // No sharing keeps files in use by Remote Desktop or the installer out of the cleanup.
    const HANDLE handle = CreateFileW(native.c_str(), DELETE | FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_DISPOSITION_INFO disposition{TRUE};
    const bool removed = GetFileInformationByHandle(handle, &info)
        && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        && isExpectedPath(handle, path)
        && SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
    CloseHandle(handle);
    return removed;
}

template <typename Match>
void scanDirectory(const QString &base, const QString &relative, bool clear, Match match, LocalCache::Result &result)
{
    const QString root = QFileInfo(base).canonicalFilePath();
    if (base.isEmpty() || root.isEmpty()) {
        result.locationUnavailable = true;
        return;
    }
    const QDir directory(QDir(root).filePath(relative));
    const QFileInfo location(directory.absolutePath());
    if (!location.exists() && !location.isSymLink())
        return;
    if (!location.isDir() || !location.isReadable()
        || location.canonicalFilePath().compare(directory.absolutePath(), Qt::CaseInsensitive) != 0) {
        result.locationUnavailable = true;
        return;
    }
    const auto entries = directory.entryInfoList(QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDir::NoSort);
    for (const QFileInfo &file : entries) {
        if (!match(file.fileName()))
            continue;
        const qint64 bytes = file.size();
        if (clear && removeUnusedFile(file.absoluteFilePath())) {
            ++result.removedFiles;
            result.removedBytes += bytes;
        } else if (!clear || QFileInfo::exists(file.absoluteFilePath())) {
            ++result.remainingFiles;
            result.remainingBytes += bytes;
            if (clear)
                ++result.failedFiles;
        }
    }
}

} // namespace

LocalCache::Result LocalCache::scan(bool clear)
{
    static const QRegularExpression bitmap(QStringLiteral("^(?:Cache[0-9]+\\.bin|bcache[0-9]*\\.bmc)$"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression installer(QStringLiteral("^RConnect-([0-9]+(?:\\.[0-9]+)+)-setup\\.exe$"),
                                               QRegularExpression::CaseInsensitiveOption);
    Result result;
    scanDirectory(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation),
                  QStringLiteral("Microsoft/Terminal Server Client/Cache"), clear,
                  [](const QString &name) { return bitmap.match(name).hasMatch(); }, result);

    const QVersionNumber running = QVersionNumber::fromString(QCoreApplication::applicationVersion());
    if (!running.isNull()) {
        const QFileInfo updates(UpdateChecker::downloadDirectory());
        scanDirectory(updates.absolutePath(), updates.fileName(), clear, [&running](const QString &name) {
            const QVersionNumber version = QVersionNumber::fromString(installer.match(name).capturedView(1));
            return !version.isNull() && QVersionNumber::compare(version, running) <= 0;
        }, result);
    }
    return result;
}
