#pragma once

#include <QtTypes>

namespace LocalCache {

struct Result {
    qint64 remainingBytes = 0;
    qint64 removedBytes = 0;
    int remainingFiles = 0;
    int removedFiles = 0;
    int failedFiles = 0;
    bool locationUnavailable = false;
};

Result scan(bool clear = false);

} // namespace LocalCache
