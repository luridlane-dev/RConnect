#include "ServerImport.h"

#include "core/Log.h"

#include <QColor>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSaveFile>
#include <QSqlQuery>
#include <QStringList>
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
bool deriveKey(const QString &passphrase, const QByteArray &salt, quint32 iterations, const wchar_t *hashAlgorithm,
               QByteArray &key)
{
    Alg alg;
    if (BCryptOpenAlgorithmProvider(&alg.h, hashAlgorithm, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return false;
    const QByteArray pass = passphrase.toUtf8();
    key.resize(kKeySize);
    return BCryptDeriveKeyPBKDF2(alg.h, reinterpret_cast<PUCHAR>(const_cast<char *>(pass.constData())), ULONG(pass.size()),
                                 reinterpret_cast<PUCHAR>(const_cast<char *>(salt.constData())), ULONG(salt.size()),
                                 iterations, reinterpret_cast<PUCHAR>(key.data()), ULONG(key.size()), 0) == 0;
}

// Payload layout: nonce (12) | tag (16) | ciphertext, or with tagLast nonce | ciphertext | tag.
// A false return means the tag did not match.
bool decrypt(const QByteArray &payload, const QByteArray &key, bool tagLast, QByteArray &plain)
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
    QByteArray tag = tagLast ? payload.right(kTagSize) : payload.mid(kNonceSize, kTagSize);
    const QByteArray cipher = tagLast ? payload.mid(kNonceSize, payload.size() - kNonceSize - kTagSize)
                                      : payload.mid(kNonceSize + kTagSize);
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

// Member lookup that ignores the case of the name ("Salt" or "salt").
QJsonValue fieldAnyCase(const QJsonObject &o, const char *name)
{
    for (auto it = o.begin(); it != o.end(); ++it)
        if (it.key().compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
            return it.value();
    return {};
}

// Base64, standard or URL-safe, with or without padding.
QByteArray decodeBase64(const QString &text)
{
    const QByteArray raw = text.trimmed().toLatin1();
    const QByteArray standard =
        QByteArray::fromBase64(raw, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    return standard.isEmpty() ? QByteArray::fromBase64(raw, QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)
                              : standard;
}

// AES-256-GCM; the result is nonce (12) | tag (16) | ciphertext.
bool encrypt(const QByteArray &plain, const QByteArray &key, QByteArray &payload)
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
    QByteArray nonce(kNonceSize, 0), tag(kTagSize, 0), cipher(plain.size(), 0);
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(nonce.data()), ULONG(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(nonce.data());
    info.cbNonce = ULONG(nonce.size());
    info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
    info.cbTag = ULONG(tag.size());
    ULONG written = 0;
    const NTSTATUS st = BCryptEncrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char *>(plain.constData())), ULONG(plain.size()),
                                      &info, nullptr, 0, reinterpret_cast<PUCHAR>(cipher.data()), ULONG(cipher.size()),
                                      &written, 0);
    BCryptDestroyKey(hKey);
    if (st != 0)
        return false;
    payload = nonce + tag + cipher.left(int(written));
    return true;
}

QJsonObject toExportJson(const ServerProfile &p)
{
    QJsonObject c;
    c[QStringLiteral("Id")] = p.id;
    c[QStringLiteral("Name")] = p.name;
    c[QStringLiteral("Address")] = p.host;
    c[QStringLiteral("Port")] = p.port;
    c[QStringLiteral("UserName")] = p.user;
    c[QStringLiteral("Domain")] = p.domain;
    c[QStringLiteral("Color")] = p.color;
    c[QStringLiteral("IsFavorite")] = p.favorite;
    c[QStringLiteral("ColorDepth")] = p.colorDepth;
    c[QStringLiteral("RedirectClipboard")] = p.clipboard;
    c[QStringLiteral("RedirectDrives")] = p.drives;
    c[QStringLiteral("AudioRedirectionMode")] = p.audio ? 0 : 2;
    c[QStringLiteral("AuthenticationLevel")] = p.ignoreCertWarnings ? 0 : 2;
    if (!p.notes.isEmpty())
        c[QStringLiteral("Notes")] = p.notes;
    return c;
}

// Opens the encrypted bundle. The old program's exact settings are not written down anywhere, so the usual
// variants are tried: the passphrase as typed or trimmed, PBKDF2 with SHA-256 (what the old program was meant
// to use), SHA-1 and SHA-512, and the tag before or after the ciphertext. Only the first that
// authenticates is accepted, so a wrong passphrase still fails. Nothing secret is logged.
bool openBundle(const QJsonObject &root, const QString &passphrase, QByteArray &plain, QString &error)
{
    const QByteArray salt = decodeBase64(fieldAnyCase(root, "Salt").toString());
    const QByteArray payload = decodeBase64(fieldAnyCase(root, "Payload").toString());
    const QJsonValue iterValue = fieldAnyCase(root, "Iterations");
    quint32 iterations = quint32(iterValue.isString() ? iterValue.toString().toInt() : iterValue.toInt(0));
    if (iterations == 0)
        iterations = 210000;
    Log::write(QStringLiteral("[import] encrypted export: keys=%1 iterations=%2 salt=%3 bytes payload=%4 bytes")
                   .arg(root.keys().join(QLatin1Char(',')))
                   .arg(iterations)
                   .arg(salt.size())
                   .arg(payload.size()));
    if (payload.size() < kNonceSize + kTagSize || salt.isEmpty()) {
        error = QStringLiteral("corrupt");
        return false;
    }

    QStringList passes = {passphrase};
    const QString trimmed = passphrase.trimmed();
    if (trimmed != passphrase)
        passes << trimmed;
    struct Hash { const wchar_t *id; const char *name; };
    const Hash hashes[] = {{BCRYPT_SHA256_ALGORITHM, "SHA-256"}, {BCRYPT_SHA1_ALGORITHM, "SHA-1"},
                           {BCRYPT_SHA512_ALGORITHM, "SHA-512"}};
    bool cryptoFailed = false;
    for (const QString &pass : std::as_const(passes)) {
        for (const Hash &h : hashes) {
            QByteArray key;
            if (!deriveKey(pass, salt, iterations, h.id, key)) {
                cryptoFailed = true;
                continue;
            }
            for (bool tagLast : {false, true}) {
                if (decrypt(payload, key, tagLast, plain)) {
                    Log::write(QStringLiteral("[import] opened with %1, tag %2").arg(QLatin1String(h.name),
                                                                                   tagLast ? QStringLiteral("last") : QStringLiteral("first")));
                    return true;
                }
            }
        }
    }
    Log::write(QStringLiteral("[import] no combination opened the export (%1)")
                   .arg(cryptoFailed ? QStringLiteral("some key derivations failed") : QStringLiteral("authentication failed")));
    error.clear();
    return false;
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

    p.notes = field(c, "Notes").toString();
    if (p.notes.isEmpty()) {
        const QJsonArray tags = field(c, "Tags").toArray();
        QStringList t;
        for (const QJsonValue &v : tags)
            t << v.toString();
        if (!t.isEmpty())
            p.notes = t.join(QStringLiteral(", "));
    }
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

bool save(const QString &path, const QList<ServerProfile> &servers, const QString &passphrase)
{
    QJsonArray connections;
    QJsonObject secrets;
    for (const ServerProfile &p : servers) {
        connections.append(toExportJson(p));
        if (!passphrase.isEmpty() && p.savePassword && !p.password.isEmpty())
            secrets.insert(p.id, p.password);
    }
    QJsonObject bundle;
    bundle[QStringLiteral("Connections")] = connections;
    if (!passphrase.isEmpty())
        bundle[QStringLiteral("Secrets")] = secrets;

    QJsonObject root;
    root[QStringLiteral("Format")] = QStringLiteral("rdpmanager-export");
    root[QStringLiteral("Version")] = 1;
    if (passphrase.isEmpty()) {
        root[QStringLiteral("Encrypted")] = false;
        root[QStringLiteral("Content")] = bundle;
    } else {
        constexpr int kIterations = 210000;
        QByteArray salt(16, 0), key, payload;
        BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(salt.data()), ULONG(salt.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!deriveKey(passphrase, salt, kIterations, BCRYPT_SHA256_ALGORITHM, key)
            || !encrypt(QJsonDocument(bundle).toJson(QJsonDocument::Compact), key, payload))
            return false;
        SecureZeroMemory(key.data(), size_t(key.size()));
        root[QStringLiteral("Encrypted")] = true;
        root[QStringLiteral("Iterations")] = kIterations;
        root[QStringLiteral("Salt")] = QString::fromLatin1(salt.toBase64());
        root[QStringLiteral("Payload")] = QString::fromLatin1(payload.toBase64());
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

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
            QByteArray plain;
            QString error;
            if (!openBundle(root, passphrase, plain, error)) {
                if (error.isEmpty()) {
                    r.status = Status::WrongPassphrase;
                } else {
                    r.error = error;
                }
                return r;
            }
            bundle = QJsonDocument::fromJson(plain).object();
            if (bundle.isEmpty()) {
                r.error = QStringLiteral("corrupt");
                return r;
            }
        }
        connections = fieldAnyCase(bundle, "Connections").toArray();
        secrets = fieldAnyCase(bundle, "Secrets").toObject();
        Log::write(QStringLiteral("[import] %1 connection(s), %2 saved password(s) in the export")
                       .arg(connections.size()).arg(secrets.size()));
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
