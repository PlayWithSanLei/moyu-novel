#pragma once

#include "core/Types.h"

namespace moyu {

struct Fingerprint {
    quint64 size = 0;
    qint64 mtimeMs = 0;
    quint64 hash = 0;

    // 缓存文件名用。不含冒号。
    QString token() const;
};

// 只读头尾各 4KB。短于 8KB 的文件整份哈希一次，避免头尾重叠把同一段算两遍。
Result<Fingerprint> fingerprintOf(const QString& path);

}  // namespace moyu
