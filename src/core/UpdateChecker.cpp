#include "UpdateChecker.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStandardPaths>
#include <QVersionNumber>

namespace {

constexpr auto kLatestRelease = "https://api.github.com/repos/luridlane-dev/RConnect/releases/latest";
constexpr qint64 kAutoCheckIntervalSecs = 12 * 3600;

QNetworkRequest makeRequest(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "RConnect/" RCONNECT_VERSION);
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000);
    return req;
}

// Only GitHub may serve the installer.
bool trustedDownload(const QUrl &url)
{
    if (url.scheme() != QLatin1String("https"))
        return false;
    const QString host = url.host().toLower();
    return host == QLatin1String("github.com") || host.endsWith(QLatin1String(".github.com"))
        || host.endsWith(QLatin1String(".githubusercontent.com"));
}

QString cleanVersion(QString tag)
{
    tag = tag.trimmed();
    if (tag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        tag.remove(0, 1);
    return tag;
}

QString updateDir() { return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("RConnect-update")); }

} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

void UpdateChecker::check()
{
    QSettings().setValue(QStringLiteral("update/lastCheck"), QDateTime::currentSecsSinceEpoch());
    QNetworkReply *reply = m_net->get(makeRequest(QUrl(QString::fromLatin1(kLatestRelease))));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        // 404: nothing has been published yet, so there is nothing newer.
        if (reply->error() == QNetworkReply::ContentNotFoundError) {
            emit checked(true, false, {}, {});
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            emit checked(false, false, {}, reply->errorString());
            return;
        }
        const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
        const QString version = cleanVersion(release.value(QLatin1String("tag_name")).toString());
        if (QVersionNumber::fromString(version).isNull()) {
            emit checked(false, false, {}, tr("The server sent an unexpected answer."));
            return;
        }
        if (!isNewer(version, QCoreApplication::applicationVersion())) {
            emit checked(true, false, {}, {});
            return;
        }

        UpdateInfo info;
        info.version = version;
        info.notes = release.value(QLatin1String("body")).toString().trimmed();
        info.pageUrl = QUrl(release.value(QLatin1String("html_url")).toString());
        const QString setupName = QStringLiteral("RConnect-%1-setup.exe").arg(version);
        const QJsonArray assets = release.value(QLatin1String("assets")).toArray();
        for (const QJsonValue &v : assets) {
            const QJsonObject asset = v.toObject();
            if (asset.value(QLatin1String("name")).toString() != setupName)
                continue;
            info.installerUrl = QUrl(asset.value(QLatin1String("browser_download_url")).toString());
            info.size = asset.value(QLatin1String("size")).toInteger();
            const QString digest = asset.value(QLatin1String("digest")).toString();
            if (digest.startsWith(QLatin1String("sha256:")))
                info.sha256 = digest.mid(7).toLower();
            break;
        }
        if (!trustedDownload(info.installerUrl))
            info.installerUrl.clear();
        // The release page is only opened in a browser; keep it on GitHub as well.
        if (!trustedDownload(info.pageUrl))
            info.pageUrl = QUrl(QStringLiteral("https://github.com/luridlane-dev/RConnect/releases/latest"));
        emit checked(true, true, info, {});
    });
}

void UpdateChecker::download(const UpdateInfo &info)
{
    if (m_download)
        return;
    if (!trustedDownload(info.installerUrl)) {
        emit downloadFailed(tr("This release has no installer."));
        return;
    }
    QDir().mkpath(updateDir());
    const QString path = QDir(updateDir()).filePath(QStringLiteral("RConnect-%1-setup.exe").arg(info.version));
    auto *file = new QFile(path, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString error = file->errorString();
        delete file;
        emit downloadFailed(error);
        return;
    }

    QNetworkRequest req = makeRequest(info.installerUrl);
    req.setRawHeader("Accept", "application/octet-stream");
    req.setTransferTimeout(60000);
    m_download = m_net->get(req);
    connect(m_download, &QNetworkReply::readyRead, file, [file, reply = m_download] { file->write(reply->readAll()); });
    connect(m_download, &QNetworkReply::downloadProgress, this, [this, info](qint64 received, qint64 total) {
        emit downloadProgress(received, total > 0 ? total : info.size);
    });
    connect(m_download, &QNetworkReply::finished, this, [this, file, path, expected = info.sha256, reply = m_download] {
        m_download = nullptr;
        reply->deleteLater();
        file->write(reply->readAll());
        file->close();
        file->deleteLater();
        const auto abort = [&](const QString &error) {
            QFile::remove(path);
            emit downloadFailed(error);
        };
        if (reply->error() == QNetworkReply::OperationCanceledError)
            return abort(tr("The download was cancelled."));
        if (reply->error() != QNetworkReply::NoError)
            return abort(reply->errorString());
        if (!expected.isEmpty()) {
            QFile in(path);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!in.open(QIODevice::ReadOnly) || !hash.addData(&in))
                return abort(in.errorString());
            if (hash.result().toHex() != expected.toLatin1())
                return abort(tr("The downloaded file does not match its checksum."));
        }
        emit downloaded(path);
    });
}

void UpdateChecker::cancelDownload()
{
    if (m_download)
        m_download->abort();
}

bool UpdateChecker::installedBySetup()
{
    return QFile::exists(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("unins000.exe")));
}

bool UpdateChecker::isNewer(const QString &candidate, const QString &current)
{
    const QVersionNumber a = QVersionNumber::fromString(candidate);
    const QVersionNumber b = QVersionNumber::fromString(current);
    return !a.isNull() && !b.isNull() && QVersionNumber::compare(a, b) > 0;
}

QString UpdateChecker::skippedVersion()
{
    return QSettings().value(QStringLiteral("update/skipped")).toString();
}

void UpdateChecker::setSkippedVersion(const QString &version)
{
    QSettings().setValue(QStringLiteral("update/skipped"), version);
}

bool UpdateChecker::autoCheckEnabled()
{
    return QSettings().value(QStringLiteral("update/auto"), true).toBool();
}

void UpdateChecker::setAutoCheckEnabled(bool on)
{
    QSettings().setValue(QStringLiteral("update/auto"), on);
}

bool UpdateChecker::autoCheckDue()
{
    const qint64 last = QSettings().value(QStringLiteral("update/lastCheck"), 0).toLongLong();
    return QDateTime::currentSecsSinceEpoch() - last >= kAutoCheckIntervalSecs;
}
