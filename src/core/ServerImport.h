#pragma once
#include "model/ServerProfile.h"

#include <QList>
#include <QString>

// Reads the server list exported by the previous program (rdpmanager-export, version 1).
// Passwords inside an encrypted export are carried over; they are re-protected by ServerProfile on save.
namespace ServerImport {

enum class Status { Ok, NeedPassphrase, WrongPassphrase, Invalid };

struct Result {
    Status status = Status::Invalid;
    QList<ServerProfile> servers;
    QString error; // only for Invalid
};

Result load(const QString &path, const QString &passphrase = {});

} // namespace ServerImport
