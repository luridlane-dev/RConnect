#pragma once
#include <QString>

// DPAPI (current user), base64 encoded.
namespace Secrets {
QString protect(const QString &plain);
QString unprotect(const QString &encoded);
}
