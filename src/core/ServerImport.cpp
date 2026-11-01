#include "ServerImport.h"

#include <QColor>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

namespace {

constexpr int kNonceSize = 12;
constexpr int kTagSize = 16;
constexpr int kKeySize = 32;

struct Alg {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~Alg() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};

// PBKDF2-HMAC-SHA256, the same as Rfc2898DeriveBytes.Pbkdf2 in the old program (passphrase as UTF-8).
bool deriveKey(const QString &passphrase, const QByteArray &salt, quint32 iterations, QByteArray &key)
{
    Alg alg;
    if (BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return false;
    const QByteArray pass = passphrase.toUtf8();
    key.resize(kKeySize);
    return BCryptDeriveKeyPBKDF2(alg.h, reinterpret_cast<PUCHAR>(const_cast<char *>(pass.constData())), ULONG(pass.size()),
                                 reinterpret_cast<PUCHAR>(const_cast<char *>(salt.constData())), ULONG(salt.size()),
                                 iterations, reinterpret_cast<PUCHAR>(key.data()), ULONG(key.size()), 0) == 0;
}

// Payload layout: nonce (12) | tag (16) | ciphertext. A false return means the tag did not match.
bool decrypt(const QByteArray &payload, const QByteArray &key, QByteArray &plain)
{
    Alg alg;
    if (BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        return false;
    if (BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0)
        return false;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptGenerateSymmetricKey(alg.h, &hKey, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(key.constData())),
                                   ULONG(key.size()), 0) != 0)
        return false;

    QByteArray nonce = payload.left(kNonceSize);
    QByteArray tag = payload.mid(kNonceSize, kTagSize);
    const QByteArray cipher = payload.mid(kNonceSize + kTagSize);
    plain.resize(cipher.size());

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(nonce.data());
    info.cbNonce = ULONG(nonce.size());
    info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
    info.cbTag = ULONG(tag.size());

    ULONG written = 0;
    const NTSTATUS st = BCryptDecrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char *>(cipher.constData())),
                                      ULONG(cipher.size()), &info, nullptr, 0,
                                      reinterpret_cast<PUCHAR>(plain.data()), ULONG(plain.size()), &written, 0);
    BCryptDestroyKey(hKey);
    if (st != 0)
        return false;
    plain.resize(int(written));
    return true;
}

// Password of the old program's database: DPAPI (current Windows user) with a fixed extra secret.
QString unprotectOld(const QString &base64)
{
    QByteArray blob = QByteArray::fromBase64(base64.toLatin1());
    if (blob.isEmpty())
        return {};
    static const QByteArray entropyText = "RdpManager.v1.credential";
    DATA_BLOB in{DWORD(blob.size()), reinterpret_cast<BYTE *>(blob.data())};
    DATA_BLOB entropy{DWORD(entropyText.size()), reinterpret_cast<BYTE *>(const_cast<char *>(entropyText.constData()))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, 0, &out))
        return {}; // saved under another Windows user
    const QString plain = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

QJsonValue field(const QJsonObject &o, const char *name)
{
    return o.value(QLatin1String(name));
}

ServerProfile toProfile(const QJsonObject &c, const QJsonObject &secrets)
{
    ServerProfile p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.host = field(c, "Address").toString().trimmed();
    p.name = field(c, "Name").toString().trimmed();
    if (p.name.isEmpty())
        p.name = p.host;
    p.port = field(c, "Port").toInt(3389);
    p.user = field(c, "UserName").toString();
    p.domain = field(c, "Domain").toString();
    p.password = secrets.value(field(c, "Id").toString()).toString();
    p.savePassword = !p.password.isEmpty();
    const QColor color(field(c, "Color").toString());
    p.color = color.isValid() ? color.name() : QString();
    p.favorite = field(c, "IsFavorite").toBool(false);

    const int depth = field(c, "ColorDepth").toInt(32);
    p.colorDepth = (depth == 16 || depth == 24 || depth == 32) ? depth : 32;
    p.clipboard = field(c, "RedirectClipboard").toBool(true);
    p.drives = field(c, "RedirectDrives").toBool(false);
    p.audio = field(c, "AudioRedirectionMode").toInt(0) == 0; // 0 = play on this computer
    p.ignoreCertWarnings = field(c, "AuthenticationLevel").toInt(0) == 0;

    const QJsonArray tags = field(c, "Tags").toArray();
    QStringList t;
    for (const QJsonValue &v : tags)
        t << v.toString();
    if (!t.isEmpty())
        p.notes = t.join(QStringLiteral(", "));
    return p;
}

// The old program's data.db: a Connections table whose Json column holds one connection each.
bool readDatabase(const QString &path, QJsonArray &connections, QJsonObject &secrets)
{
    const QString name = QStringLiteral("rconnect-import");
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery q(db);
            ok = q.exec(QStringLiteral("SELECT Id, Json FROM Connections ORDER BY SortOrder, Name"));
            while (ok && q.next()) {
                QJsonObject c = QJsonDocument::fromJson(q.value(1).toString().toUtf8()).object();
                if (c.isEmpty())
                    continue;
                const QString id = q.value(0).toString();
                c.insert(QStringLiteral("Id"), id);
                const QString password = unprotectOld(c.value(QLatin1String("ProtectedPassword")).toString());
                if (!password.isEmpty())
                    secrets.insert(id, password);
                connections.append(c);
            }
        }
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

} // namespace

namespace ServerImport {

Result load(const QString &path, const QString &passphrase)
{
    Result r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = f.errorString();
        return r;
    }
    const QByteArray content = f.readAll();
    f.close();

    QJsonArray connections;
    QJsonObject secrets;

    if (content.startsWith("SQLite format 3")) {
        if (!readDatabase(path, connections, secrets)) {
            r.error = QStringLiteral("database");
            return r;
        }
        for (const QJsonValue &v : std::as_const(connections)) {
            const ServerProfile p = toProfile(v.toObject(), secrets);
            if (p.isValid())
                r.servers.append(p);
        }
        r.status = Status::Ok;
        return r;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(content);
    if (doc.isArray()) { // plain list of connections
        connections = doc.array();
    } else if (doc.isObject() && doc.object().value(QLatin1String("Format")).toString() == QLatin1String("rdpmanager-export")) {
        const QJsonObject root = doc.object();
        QJsonObject bundle = root.value(QLatin1String("Content")).toObject();
        if (root.value(QLatin1String("Encrypted")).toBool()) {
            if (passphrase.isEmpty()) {
                r.status = Status::NeedPassphrase;
                return r;
            }
            const QByteArray salt = QByteArray::fromBase64(root.value(QLatin1String("Salt")).toString().toLatin1());
            const QByteArray payload = QByteArray::fromBase64(root.value(QLatin1String("Payload")).toString().toLatin1());
            const quint32 iterations = quint32(root.value(QLatin1String("Iterations")).toInt(210000));
            if (payload.size() < kNonceSize + kTagSize || salt.isEmpty()) {
                r.error = QStringLiteral("corrupt");
                return r;
            }
            QByteArray key, plain;
            if (!deriveKey(passphrase, salt, iterations, key)) {
                r.error = QStringLiteral("crypto");
                return r;
            }
            if (!decrypt(payload, key, plain)) {
                r.status = Status::WrongPassphrase;
                return r;
            }
            bundle = QJsonDocument::fromJson(plain).object();
        }
        connections = bundle.value(QLatin1String("Connections")).toArray();
        secrets = bundle.value(QLatin1String("Secrets")).toObject();
    } else {
        r.error = QStringLiteral("format");
        return r;
    }

    for (const QJsonValue &v : connections) {
        const ServerProfile p = toProfile(v.toObject(), secrets);
        if (p.isValid())
            r.servers.append(p);
    }
    r.status = Status::Ok;
    return r;
}

} // namespace ServerImport
