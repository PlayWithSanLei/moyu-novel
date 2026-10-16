#include "parser/ZipArchive.h"

#include <QFile>
#include <QUrl>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <utility>

namespace moyu {
namespace {

QString normalizeStoredName(QString name)
{
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1Char('/'))) {
        name.remove(0, 1);
    }
    QStringList parts;
    for (const QString& part : name.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String(".")) {
            continue;
        }
        if (part == QLatin1String("..")) {
            if (parts.isEmpty()) {
                return {};
            }
            parts.removeLast();
            continue;
        }
        parts.push_back(part);
    }
    return parts.join(QLatin1Char('/'));
}

}  // namespace

ZipArchive::~ZipArchive()
{
    if (m_reader != nullptr) {
        mz_zip_reader_close(m_reader);
        mz_zip_reader_delete(&m_reader);
    }
}

ZipArchive::ZipArchive(ZipArchive&& other) noexcept
    : m_reader(other.m_reader)
    , m_entries(std::move(other.m_entries))
    , m_readBytes(other.m_readBytes)
{
    other.m_reader = nullptr;
    other.m_readBytes = 0;
}

ZipArchive& ZipArchive::operator=(ZipArchive&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    if (m_reader != nullptr) {
        mz_zip_reader_close(m_reader);
        mz_zip_reader_delete(&m_reader);
    }
    m_reader = other.m_reader;
    m_entries = std::move(other.m_entries);
    m_readBytes = other.m_readBytes;
    other.m_reader = nullptr;
    other.m_readBytes = 0;
    return *this;
}

Result<ZipArchive> ZipArchive::open(const QString& path)
{
    ZipArchive archive;
    archive.m_reader = mz_zip_reader_create();
    if (archive.m_reader == nullptr) {
        return std::unexpected(makeError(QStringLiteral("无法打开压缩包")));
    }
    const QByteArray encoded = QFile::encodeName(path);
    if (mz_zip_reader_open_file(archive.m_reader, encoded.constData()) != MZ_OK) {
        mz_zip_reader_delete(&archive.m_reader);
        return std::unexpected(makeError(QStringLiteral("无法打开压缩包")));
    }

    qint64 declaredTotal = 0;
    int32_t err = mz_zip_reader_goto_first_entry(archive.m_reader);
    int count = 0;
    while (err == MZ_OK) {
        if (count >= kMaxZipEntries) {
            mz_zip_reader_close(archive.m_reader);
            mz_zip_reader_delete(&archive.m_reader);
            return std::unexpected(makeError(QStringLiteral("压缩包条目太多")));
        }
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(archive.m_reader, &info) != MZ_OK || info == nullptr || info->filename == nullptr) {
            err = mz_zip_reader_goto_next_entry(archive.m_reader);
            continue;
        }
        if (info->uncompressed_size > static_cast<int64_t>(kMaxZipEntryBytes) || info->uncompressed_size < 0) {
            mz_zip_reader_close(archive.m_reader);
            mz_zip_reader_delete(&archive.m_reader);
            return std::unexpected(makeError(QStringLiteral("压缩包里有过大的文件")));
        }
        void* zipHandle = nullptr;
        mz_zip_reader_get_zip_handle(archive.m_reader, &zipHandle);
        const int64_t cdPos = zipHandle != nullptr ? mz_zip_get_entry(zipHandle) : -1;
        declaredTotal += info->uncompressed_size;
        if (declaredTotal > static_cast<qint64>(kMaxZipTotalBytes)) {
            mz_zip_reader_close(archive.m_reader);
            mz_zip_reader_delete(&archive.m_reader);
            return std::unexpected(makeError(QStringLiteral("压缩包展开后太大")));
        }
        const QString stored = QString::fromUtf8(info->filename);
        const QString key = normalizeStoredName(stored);
        if (!key.isEmpty()) {
            Entry entry;
            entry.storedName = stored;
            entry.declaredSize = info->uncompressed_size;
            entry.cdPos = cdPos;
            archive.m_entries.insert(key, std::move(entry));
        }
        ++count;
        err = mz_zip_reader_goto_next_entry(archive.m_reader);
    }
    return archive;
}

bool ZipArchive::contains(const QString& name) const
{
    return m_entries.contains(normalizeStoredName(name));
}

QStringList ZipArchive::names() const
{
    return QStringList(m_entries.keyBegin(), m_entries.keyEnd());
}

Result<QByteArray> ZipArchive::read(const QString& name)
{
    const QString key = normalizeStoredName(name);
    const auto found = m_entries.constFind(key);
    if (found == m_entries.cend()) {
        return std::unexpected(makeError(QStringLiteral("压缩包里缺少文件")));
    }
    void* zipHandle = nullptr;
    if (mz_zip_reader_get_zip_handle(m_reader, &zipHandle) != MZ_OK || zipHandle == nullptr) {
        return std::unexpected(makeError(QStringLiteral("压缩包里缺少文件")));
    }
    if (mz_zip_entry_is_open(zipHandle) == MZ_OK) {
        mz_zip_entry_close(zipHandle);
    }
    if (found->cdPos < 0 || mz_zip_goto_entry(zipHandle, found->cdPos) != MZ_OK) {
        const QByteArray stored = found->storedName.toUtf8();
        if (mz_zip_locate_entry(zipHandle, stored.constData(), 0) != MZ_OK) {
            return std::unexpected(makeError(QStringLiteral("压缩包里缺少文件")));
        }
    }
    mz_zip_file* info = nullptr;
    if (mz_zip_entry_get_info(zipHandle, &info) != MZ_OK || info == nullptr) {
        return std::unexpected(makeError(QStringLiteral("压缩包条目损坏")));
    }
    // 头里的大小可以比实际展开小。读的时候仍按真实字节数封顶。
    if (mz_zip_entry_read_open(zipHandle, 0, nullptr) != MZ_OK) {
        return std::unexpected(makeError(QStringLiteral("无法解压文件")));
    }
    QByteArray out;
    if (info->uncompressed_size > 0 && info->uncompressed_size <= static_cast<int64_t>(kMaxZipEntryBytes)) {
        out.reserve(static_cast<qsizetype>(info->uncompressed_size));
    }
    QByteArray chunk(64 * 1024, Qt::Uninitialized);
    while (true) {
        const int32_t got = mz_zip_entry_read(zipHandle, chunk.data(), static_cast<int32_t>(chunk.size()));
        if (got < 0) {
            mz_zip_entry_close(zipHandle);
            return std::unexpected(makeError(QStringLiteral("解压失败")));
        }
        if (got == 0) {
            break;
        }
        if (m_readBytes > kMaxZipTotalBytes - static_cast<quint64>(got) ||
            static_cast<quint64>(out.size()) > kMaxZipEntryBytes - static_cast<quint64>(got)) {
            mz_zip_entry_close(zipHandle);
            return std::unexpected(makeError(QStringLiteral("压缩包展开后太大")));
        }
        out.append(chunk.constData(), got);
        m_readBytes += static_cast<quint64>(got);
    }
    mz_zip_entry_close(zipHandle);
    return out;
}

QString resolveZipPath(const QString& baseDir, const QString& href)
{
    const QString withoutFragment = href.section(QLatin1Char('#'), 0, 0);
    QString path = QUrl::fromPercentEncoding(withoutFragment.toUtf8());
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.startsWith(QLatin1String("//")) || path.contains(QLatin1String("://"))) {
        return {};
    }
    QString combined;
    if (path.startsWith(QLatin1Char('/'))) {
        combined = path.mid(1);
    } else if (baseDir.isEmpty()) {
        combined = path;
    } else {
        combined = baseDir + QLatin1Char('/') + path;
    }
    QStringList parts;
    for (const QString& part : combined.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String(".")) {
            continue;
        }
        if (part == QLatin1String("..")) {
            if (parts.isEmpty()) {
                return {};
            }
            parts.removeLast();
            continue;
        }
        parts.push_back(part);
    }
    return parts.join(QLatin1Char('/'));
}

}  // namespace moyu
