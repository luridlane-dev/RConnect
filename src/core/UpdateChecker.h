#pragma once
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// A newer release on GitHub.
struct UpdateInfo {
    QString version;
    QString notes;
    QUrl pageUrl;      // the release page
    QUrl installerUrl; // RConnect-<version>-setup.exe; empty when the release has none
    QString sha256;    // lower case hex; empty when GitHub gave no digest
    qint64 size = 0;
};

// Looks for a newer release of RConnect on GitHub and downloads its installer.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // Finishes with checked(). A failed request has ok = false and a message in error.
    void check();
    // Downloads info.installerUrl and checks its SHA-256; finishes with downloaded() or downloadFailed().
    void download(const UpdateInfo &info);
    void cancelDownload();

    // True when this copy was set up by the installer (the installer can replace it in place).
    static bool installedBySetup();
    static bool isNewer(const QString &candidate, const QString &current);

    static QString skippedVersion();
    static void setSkippedVersion(const QString &version);
    static bool autoCheckEnabled();
    static void setAutoCheckEnabled(bool on);
    // Whether the automatic check is due (once every 12 hours).
    static bool autoCheckDue();

signals:
    void checked(bool ok, bool available, const UpdateInfo &info, const QString &error);
    void downloadProgress(qint64 received, qint64 total);
    void downloaded(const QString &installerPath);
    void downloadFailed(const QString &error);

private:
    QNetworkAccessManager *m_net;
    QNetworkReply *m_download = nullptr;
};
