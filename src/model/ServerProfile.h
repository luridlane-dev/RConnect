#pragma once
#include <QJsonObject>
#include <QSize>
#include <QString>

struct ServerProfile {
    QString id;
    QString name;
    QString host;
    int port = 3389;
    QString user;
    QString domain;
    QString password;          // DPAPI-encrypted on disk
    bool savePassword = true;
    QString color;             // empty = pick on save
    QString notes;             // DPAPI-encrypted on disk
    QString country;           // ISO code from the offline IP table
    QString countryHost;       // host the country was looked up for

    int colorDepth = 32;       // 16 / 24 / 32
    QString resolution;        // empty = fit window, else "1920x1080"
    bool clipboard = true;
    bool drives = false;
    bool audio = true;
    bool ignoreCertWarnings = true;
    bool monitor = true;       // port check once a minute
    bool checkSessions = true; // who is connected, over WinRM with the saved password

    bool isValid() const { return !host.trimmed().isEmpty(); }
    QString displayUser() const;
    QSize fixedResolution() const;
    QJsonObject toJson() const;
    static ServerProfile fromJson(const QJsonObject &o);
};
