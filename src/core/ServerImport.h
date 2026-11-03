#pragma once
#include "model/ServerProfile.h"

#include <QList>
#include <QString>

// Reads and writes the server list file (rdpmanager-export, version 1), the format of the previous program.
// The Android app reads and writes the same file. Passwords inside an encrypted export are carried over; they
// are re-protected by ServerProfile on save.
namespace ServerImport {

enum class Status { Ok, NeedPassphrase, WrongPassphrase, Invalid };

struct Result {
    Status status = Status::Invalid;
    QList<ServerProfile> servers;
    QString error; // only for Invalid
};

Result load(const QString &path, const QString &passphrase = {});

// Writes `servers` to `path`. With a passphrase the file is encrypted (PBKDF2-SHA256, AES-256-GCM) and carries
// the saved passwords; without one it holds no passwords. Returns false if the file could not be written.
bool save(const QString &path, const QList<ServerProfile> &servers, const QString &passphrase = {});

} // namespace ServerImport
