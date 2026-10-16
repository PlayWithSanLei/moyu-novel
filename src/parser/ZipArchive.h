#pragma once

#include "core/Types.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

namespace moyu {

// 单个条目和全部解压都有上限，避免 zip 炸弹把内存撑满。
inline constexpr quint64 kMaxZipEntryBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr quint64 kMaxZipTotalBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr int kMaxZipEntries = 20000;

class ZipArchive {
public:
    ZipArchive() = default;
    ~ZipArchive();

    ZipArchive(const ZipArchive&) = delete;
    ZipArchive& operator=(const ZipArchive&) = delete;
    ZipArchive(ZipArchive&& other) noexcept;
    ZipArchive& operator=(ZipArchive&& other) noexcept;

    static Result<ZipArchive> open(const QString& path);

    bool contains(const QString& name) const;
    QStringList names() const;
    Result<QByteArray> read(const QString& name);

private:
    struct Entry {
        QString storedName;
        qint64 declaredSize = 0;
        // 中央目录里这一条的位置。读取时直接 goto，不再按文件名线性查找。
        qint64 cdPos = -1;
    };

    void* m_reader = nullptr;
    QHash<QString, Entry> m_entries;
    quint64 m_readBytes = 0;
};

// 去掉片段和百分号编码，拒绝跳出压缩包根目录的路径。失败返回空字符串。
QString resolveZipPath(const QString& baseDir, const QString& href);

}  // namespace moyu
