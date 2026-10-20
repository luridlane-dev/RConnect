#include "Secrets.h"

#include <QByteArray>
#include <windows.h>
#include <wincrypt.h>

namespace Secrets {

QString protect(const QString &plain)
{
    if (plain.isEmpty())
        return {};
    QByteArray in = plain.toUtf8();
    DATA_BLOB src{static_cast<DWORD>(in.size()), reinterpret_cast<BYTE *>(in.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&src, L"RConnect", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    QByteArray enc(reinterpret_cast<const char *>(out.pbData), static_cast<qsizetype>(out.cbData));
    LocalFree(out.pbData);
    return QString::fromLatin1(enc.toBase64());
}

QString unprotect(const QString &encoded)
{
    if (encoded.isEmpty())
        return {};
    QByteArray in = QByteArray::fromBase64(encoded.toLatin1());
    DATA_BLOB src{static_cast<DWORD>(in.size()), reinterpret_cast<BYTE *>(in.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    QString plain = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData),
                                      static_cast<qsizetype>(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

} // namespace Secrets
