#pragma once

#include "core/Types.h"

#include <memory>

class QFile;

namespace moyu {

// QFile 是 QObject，不能随便跨线程销毁。只在创建它的线程上打开和析构。
// 空文件算错误：没有正文可以映射。
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    static Result<MappedFile> open(const QString& path);

    const char* data() const { return reinterpret_cast<const char*>(m_data); }
    quint64 size() const { return m_size; }

private:
    void reset() noexcept;

    std::unique_ptr<QFile> m_file;
    unsigned char* m_data = nullptr;
    quint64 m_size = 0;
};

}  // namespace moyu
