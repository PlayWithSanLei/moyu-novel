#include "core/MappedFile.h"

#include <QFile>

#include <limits>
#include <utility>

namespace moyu {

MappedFile::~MappedFile()
{
    reset();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : m_file(std::move(other.m_file))
    , m_data(other.m_data)
    , m_size(other.m_size)
{
    other.m_data = nullptr;
    other.m_size = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept
{
    if (this != &other) {
        reset();
        m_file = std::move(other.m_file);
        m_data = other.m_data;
        m_size = other.m_size;
        other.m_data = nullptr;
        other.m_size = 0;
    }
    return *this;
}

void MappedFile::reset() noexcept
{
    if (m_file != nullptr && m_data != nullptr) {
        m_file->unmap(m_data);
    }
    m_data = nullptr;
    m_size = 0;
    m_file.reset();
}

Result<MappedFile> MappedFile::open(const QString& path)
{
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::ReadOnly)) {
        return std::unexpected(makeError(QStringLiteral("无法打开文件")));
    }
    const qint64 size = file->size();
    if (size < 0) {
        return std::unexpected(makeError(QStringLiteral("无法读取文件大小")));
    }
    if (size == 0) {
        return std::unexpected(makeError(QStringLiteral("文件是空的")));
    }
    if (static_cast<quint64>(size) > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        return std::unexpected(makeError(QStringLiteral("文件太大")));
    }
    unsigned char* mapped = file->map(0, size);
    if (mapped == nullptr) {
        return std::unexpected(makeError(QStringLiteral("无法映射文件")));
    }

    MappedFile result;
    result.m_size = static_cast<quint64>(size);
    result.m_data = mapped;
    result.m_file = std::move(file);
    return result;
}

}  // namespace moyu
