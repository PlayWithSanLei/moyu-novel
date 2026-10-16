#include "core/Fingerprint.h"

#include <QFile>
#include <QFileInfo>

#include <xxhash.h>

namespace moyu {
namespace {

constexpr qint64 kEdgeBytes = 4096;

}  // namespace

QString Fingerprint::token() const
{
    const QString hex = QString::number(hash, 16).rightJustified(16, QLatin1Char('0'));
    return QString::number(size) + QLatin1Char('_') + QString::number(mtimeMs) + QLatin1Char('_') + hex;
}

Result<Fingerprint> fingerprintOf(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(makeError(QStringLiteral("无法读取文件")));
    }
    const qint64 size = file.size();
    if (size < 0) {
        return std::unexpected(makeError(QStringLiteral("无法读取文件大小")));
    }

    QByteArray hashed;
    if (size <= kEdgeBytes * 2) {
        hashed = file.readAll();
        if (hashed.size() != size) {
            return std::unexpected(makeError(QStringLiteral("读取文件失败")));
        }
    } else {
        const QByteArray head = file.read(kEdgeBytes);
        if (head.size() != kEdgeBytes || !file.seek(size - kEdgeBytes)) {
            return std::unexpected(makeError(QStringLiteral("读取文件失败")));
        }
        const QByteArray tail = file.read(kEdgeBytes);
        if (tail.size() != kEdgeBytes) {
            return std::unexpected(makeError(QStringLiteral("读取文件失败")));
        }
        hashed.reserve(head.size() + tail.size());
        hashed.append(head);
        hashed.append(tail);
    }

    Fingerprint fingerprint;
    fingerprint.size = static_cast<quint64>(size);
    fingerprint.mtimeMs = QFileInfo(file).lastModified().toMSecsSinceEpoch();
    fingerprint.hash = static_cast<quint64>(
        XXH3_64bits(hashed.constData(), static_cast<std::size_t>(hashed.size())));
    return fingerprint;
}

}  // namespace moyu
