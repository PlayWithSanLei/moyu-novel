#include "core/Encoding.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iconv.h>
#include <limits>
#include <unistd.h>
#include <utility>
#include <vector>

#include <uchardet.h>

namespace moyu {
namespace {

constexpr quint64 kSampleBytes = 64 * 1024;
constexpr char kReplacement[] = "\xEF\xBF\xBD";

class Detector {
public:
    Detector()
        : m_detector(uchardet_new())
    {
    }
    ~Detector()
    {
        if (m_detector != nullptr) {
            uchardet_delete(m_detector);
        }
    }
    Detector(const Detector&) = delete;
    Detector& operator=(const Detector&) = delete;

    uchardet_t get() const { return m_detector; }

private:
    uchardet_t m_detector = nullptr;
};

class Iconv {
public:
    explicit Iconv(const char* from)
        : m_descriptor(iconv_open("UTF-8", from))
    {
    }
    ~Iconv()
    {
        if (m_descriptor != reinterpret_cast<iconv_t>(-1)) {
            iconv_close(m_descriptor);
        }
    }
    Iconv(const Iconv&) = delete;
    Iconv& operator=(const Iconv&) = delete;

    bool ok() const { return m_descriptor != reinterpret_cast<iconv_t>(-1); }
    iconv_t get() const { return m_descriptor; }
    void reset() { iconv(m_descriptor, nullptr, nullptr, nullptr, nullptr); }

private:
    iconv_t m_descriptor;
};

QString canonicalName(QString name)
{
    name = name.trimmed().toUpper();
    name.replace(QLatin1Char('_'), QLatin1Char('-'));
    if (name == QLatin1String("UTF-8") || name == QLatin1String("ASCII") ||
        name == QLatin1String("US-ASCII") || name == QLatin1String("ANSI-X3.4-1968")) {
        return QStringLiteral("UTF-8");
    }
    if (name == QLatin1String("GB18030") || name == QLatin1String("GBK") ||
        name == QLatin1String("GB2312") || name == QLatin1String("GB-2312")) {
        return QStringLiteral("GB18030");
    }
    if (name == QLatin1String("BIG5") || name == QLatin1String("BIG-5") ||
        name == QLatin1String("BIG5-HKSCS") || name == QLatin1String("CP950")) {
        return QStringLiteral("BIG5");
    }
    if (name == QLatin1String("UTF-16LE") || name == QLatin1String("UTF-16-LE")) {
        return QStringLiteral("UTF-16LE");
    }
    if (name == QLatin1String("UTF-16BE") || name == QLatin1String("UTF-16-BE")) {
        return QStringLiteral("UTF-16BE");
    }
    if (name == QLatin1String("UTF-16")) {
        return QStringLiteral("UTF-16");
    }
    return {};
}

QString iconvNameFor(const QString& canonical)
{
    if (canonical == QLatin1String("GB18030")) {
        return QStringLiteral("GB18030");
    }
    if (canonical == QLatin1String("BIG5")) {
        // BIG5-HKSCS 是 Big5 的超集，香港增补和欧元符号也能解开。探测名仍然记 BIG5。
        return QStringLiteral("BIG5-HKSCS");
    }
    if (canonical == QLatin1String("UTF-16LE")) {
        return QStringLiteral("UTF-16LE");
    }
    if (canonical == QLatin1String("UTF-16BE")) {
        return QStringLiteral("UTF-16BE");
    }
    return {};
}

NewlineCut cutFor(const QString& canonical)
{
    if (canonical == QLatin1String("UTF-16LE")) {
        return NewlineCut::Utf16Le;
    }
    if (canonical == QLatin1String("UTF-16BE")) {
        return NewlineCut::Utf16Be;
    }
    return NewlineCut::Bytes;
}

DetectedEncoding finish(const QString& canonical, quint64 offset)
{
    DetectedEncoding encoding;
    encoding.canonical = canonical;
    encoding.iconvName = iconvNameFor(canonical);
    encoding.bodyOffset = offset;
    encoding.cut = cutFor(canonical);
    return encoding;
}

// 没有 BOM 时，ASCII 区的 0 在奇数位更像 LE，在偶数位更像 BE。
QString utf16Endian(const char* data, quint64 size)
{
    quint64 evenNulls = 0;
    quint64 oddNulls = 0;
    const quint64 sample = std::min(size, kSampleBytes);
    const quint64 aligned = sample - (sample % 2);
    for (quint64 i = 0; i + 1 < aligned; i += 2) {
        if (static_cast<unsigned char>(data[i]) == 0) {
            ++evenNulls;
        }
        if (static_cast<unsigned char>(data[i + 1]) == 0) {
            ++oddNulls;
        }
    }
    return oddNulls >= evenNulls ? QStringLiteral("UTF-16LE") : QStringLiteral("UTF-16BE");
}

bool writeAll(QFile& file, const char* data, qsizetype size)
{
    qsizetype done = 0;
    while (done < size) {
        const qint64 wrote = file.write(data + done, size - done);
        if (wrote < 0) {
            return false;
        }
        done += static_cast<qsizetype>(wrote);
    }
    return true;
}

// 断电无法在测试里复现。rename 前把数据和元数据都落到盘上，避免留下「大小对、内容全是 0」的缓存。
bool syncFile(QFile& file)
{
    if (!file.flush()) {
        return false;
    }
    const int fd = file.handle();
    if (fd < 0) {
        return false;
    }
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0) {
        return true;
    }
#endif
    return ::fsync(fd) == 0;
}

// 跨块记住末尾的 \r，这样 \r\n 被切在两块中间时也只写出一个 \n。
class Utf8NewlineWriter {
public:
    explicit Utf8NewlineWriter(QFile& file)
        : m_file(file)
    {
    }

    Result<void> write(const char* data, qsizetype size)
    {
        if (size < 0) {
            return std::unexpected(makeError(QStringLiteral("写入缓存失败")));
        }
        if (size == 0) {
            return {};
        }
        if (!m_pendingCr && std::memchr(data, '\r', static_cast<std::size_t>(size)) == nullptr) {
            if (!writeAll(m_file, data, size)) {
                return std::unexpected(makeError(QStringLiteral("写入缓存失败")));
            }
            return {};
        }
        QByteArray filtered;
        filtered.reserve(size + 1);
        for (qsizetype i = 0; i < size; ++i) {
            const char byte = data[i];
            if (m_pendingCr) {
                m_pendingCr = false;
                filtered.append('\n');
                if (byte == '\n') {
                    continue;
                }
            }
            if (byte == '\r') {
                m_pendingCr = true;
                continue;
            }
            filtered.append(byte);
        }
        if (filtered.isEmpty()) {
            return {};
        }
        if (!writeAll(m_file, filtered.constData(), filtered.size())) {
            return std::unexpected(makeError(QStringLiteral("写入缓存失败")));
        }
        return {};
    }

    Result<void> finish()
    {
        if (!m_pendingCr) {
            return {};
        }
        m_pendingCr = false;
        if (!writeAll(m_file, "\n", 1)) {
            return std::unexpected(makeError(QStringLiteral("写入缓存失败")));
        }
        return {};
    }

private:
    QFile& m_file;
    bool m_pendingCr = false;
};

Result<void> commitPartial(QFile& output, const QString& partial, const QString& destination)
{
    if (!syncFile(output)) {
        output.remove();
        return std::unexpected(makeError(QStringLiteral("无法把缓存写到磁盘")));
    }
    output.close();
    if (QFile::exists(destination)) {
        QFile::remove(destination);
    }
    if (!QFile::rename(partial, destination)) {
        QFile::remove(partial);
        return std::unexpected(makeError(QStringLiteral("无法保存缓存")));
    }
    return {};
}

// 返回本块实际吃掉的输入字节。后面还有数据时，末尾不完整的序列留给下一块，不写成 U+FFFD。
Result<quint64> transcodeChunk(Iconv& converter,
                                const char* data,
                                quint64 size,
                                NewlineCut cut,
                                bool atEof,
                                Utf8NewlineWriter& writer)
{
    if (size > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        return std::unexpected(makeError(QStringLiteral("分块太大")));
    }
    // iconv 的入参是 char**。这些编码下它只前进指针，不改输入字节。映射来的内存是 const。
    char* input = const_cast<char*>(data);
    std::size_t inputLeft = static_cast<std::size_t>(size);
    std::vector<char> buffer(std::max<std::size_t>(inputLeft * 4 + 16, 64));
    constexpr std::size_t kBufferCap = 16 * 1024 * 1024;

    while (inputLeft > 0) {
        char* outputPointer = buffer.data();
        std::size_t outputLeft = buffer.size();
        const std::size_t converted = iconv(converter.get(), &input, &inputLeft, &outputPointer, &outputLeft);
        const auto produced = static_cast<qsizetype>(buffer.size() - outputLeft);
        if (produced > 0) {
            auto written = writer.write(buffer.data(), produced);
            if (!written) {
                return std::unexpected(written.error());
            }
        }
        if (converted != static_cast<std::size_t>(-1)) {
            break;
        }
        if (errno == E2BIG) {
            if (produced == 0) {
                if (buffer.size() >= kBufferCap) {
                    return std::unexpected(makeError(QStringLiteral("转码输出过大")));
                }
                buffer.resize(std::min(buffer.size() * 2, kBufferCap));
            }
            continue;
        }
        if (errno == EILSEQ) {
            auto written = writer.write(kReplacement, 3);
            if (!written) {
                return std::unexpected(written.error());
            }
            std::size_t skip = 1;
            if (cut != NewlineCut::Bytes && inputLeft >= 2) {
                skip = 2;
            }
            if (skip > inputLeft) {
                skip = inputLeft;
            }
            input += skip;
            inputLeft -= skip;
            converter.reset();
            continue;
        }
        if (errno == EINVAL) {
            if (!atEof) {
                // 硬切在多字节字符中间。状态机清掉，剩下的字节跟下一块一起再解。
                converter.reset();
                break;
            }
            auto written = writer.write(kReplacement, 3);
            if (!written) {
                return std::unexpected(written.error());
            }
            inputLeft = 0;
            break;
        }
        return std::unexpected(makeError(QStringLiteral("转码失败")));
    }
    if (inputLeft > static_cast<std::size_t>(size)) {
        return std::unexpected(makeError(QStringLiteral("转码失败")));
    }
    return size - static_cast<quint64>(inputLeft);
}

Result<DetectedEncoding> utf8OrFallback(const char* data, quint64 size, quint64 offset, bool checkUtf8)
{
    if (checkUtf8 && offset <= size && !utf8WellFormed(data + offset, size - offset)) {
        return finish(QStringLiteral("GB18030"), offset);
    }
    return finish(QStringLiteral("UTF-8"), offset);
}

}  // namespace

Result<DetectedEncoding> detectEncoding(const char* data, quint64 size, bool checkUtf8)
{
    if (data == nullptr || size == 0) {
        return std::unexpected(makeError(QStringLiteral("文件是空的")));
    }
    if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF) {
        return utf8OrFallback(data, size, 3, checkUtf8);
    }
    if (size >= 2 && static_cast<unsigned char>(data[0]) == 0xFF &&
        static_cast<unsigned char>(data[1]) == 0xFE) {
        return finish(QStringLiteral("UTF-16LE"), 2);
    }
    if (size >= 2 && static_cast<unsigned char>(data[0]) == 0xFE &&
        static_cast<unsigned char>(data[1]) == 0xFF) {
        return finish(QStringLiteral("UTF-16BE"), 2);
    }

    Detector detector;
    if (detector.get() == nullptr) {
        return std::unexpected(makeError(QStringLiteral("无法创建编码探测器")));
    }
    const quint64 sample = std::min(size, kSampleBytes);
    if (uchardet_handle_data(detector.get(), data, static_cast<std::size_t>(sample)) != 0) {
        return std::unexpected(makeError(QStringLiteral("编码探测失败")));
    }
    uchardet_data_end(detector.get());
    const QString canonical = canonicalName(QString::fromLatin1(uchardet_get_charset(detector.get())));
    if (canonical.isEmpty()) {
        return std::unexpected(makeError(QStringLiteral("无法识别编码")));
    }
    if (canonical == QLatin1String("UTF-16")) {
        return finish(utf16Endian(data, size), 0);
    }
    if (canonical == QLatin1String("UTF-8")) {
        return utf8OrFallback(data, size, 0, checkUtf8);
    }
    if (iconvNameFor(canonical).isEmpty()) {
        return std::unexpected(makeError(QStringLiteral("无法识别编码：") + canonical));
    }
    return finish(canonical, 0);
}

QString htmlMetaCharset(const char* data, quint64 size)
{
    if (data == nullptr || size == 0) {
        return {};
    }
    const quint64 sample = std::min(size, static_cast<quint64>(8192));
    const QString head = QString::fromLatin1(data, static_cast<qsizetype>(sample));
    QRegularExpression meta(QStringLiteral("<meta\\b[^>]*charset\\s*=\\s*[\"']?([^\"'>\\s/]+)"),
                            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = meta.match(head);
    if (!match.hasMatch()) {
        return {};
    }
    return match.captured(1).trimmed();
}

Result<DetectedEncoding> encodingFromDeclaredName(const QString& name)
{
    const QString canonical = canonicalName(name);
    if (canonical.isEmpty()) {
        return std::unexpected(makeError(QStringLiteral("无法识别编码")));
    }
    if (canonical == QLatin1String("UTF-8")) {
        return finish(QStringLiteral("UTF-8"), 0);
    }
    if (iconvNameFor(canonical).isEmpty()) {
        return std::unexpected(makeError(QStringLiteral("无法识别编码")));
    }
    return finish(canonical, 0);
}

namespace {

std::atomic<quint64> g_transcodeCalls{0};

}  // namespace

quint64 transcodeCallCount()
{
    return g_transcodeCalls.load();
}

void resetTranscodeCallCount()
{
    g_transcodeCalls.store(0);
}

Result<void> transcodeToCacheFile(const char* data,
                                   quint64 size,
                                   const DetectedEncoding& encoding,
                                   const QString& destination,
                                   quint64 chunkTarget,
                                   const std::atomic<bool>& cancel)
{
    g_transcodeCalls.fetch_add(1);
    if (data == nullptr || size == 0) {
        return std::unexpected(makeError(QStringLiteral("文件是空的")));
    }
    if (encoding.iconvName.isEmpty()) {
        return std::unexpected(makeError(QStringLiteral("这个编码不需要转码")));
    }
    QDir().mkpath(QFileInfo(destination).absolutePath());
    const QString partial = destination + QStringLiteral(".partial");
    QFile output(partial);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return std::unexpected(makeError(QStringLiteral("无法写入缓存")));
    }

    const QByteArray from = encoding.iconvName.toLatin1();
    Iconv converter(from.constData());
    if (!converter.ok()) {
        output.remove();
        return std::unexpected(makeError(QStringLiteral("无法用 iconv 打开编码 ") + encoding.iconvName));
    }

    Utf8NewlineWriter writer(output);
    const char* cursor = data;
    const char* end = data + size;
    while (cursor < end) {
        if (cancel.load()) {
            output.remove();
            return std::unexpected(cancelledError());
        }
        quint64 chunk = findChunkEnd(cursor, end, chunkTarget, encoding.cut);
        if (chunk == 0) {
            output.remove();
            return std::unexpected(makeError(QStringLiteral("转码分块没有前进")));
        }
        bool atEof = cursor + chunk >= end;
        auto consumed = transcodeChunk(converter, cursor, chunk, encoding.cut, atEof, writer);
        if (!consumed) {
            output.remove();
            return std::unexpected(consumed.error());
        }
        if (*consumed == 0) {
            // 整块都是半个字符。再带上最多 4 个字节重试，仍然不动就跳过 1 字节，避免死循环。
            const quint64 remain = static_cast<quint64>(end - cursor);
            if (chunk < remain) {
                const quint64 extra = std::min<quint64>(4, remain - chunk);
                const bool retryEof = chunk + extra >= remain;
                consumed = transcodeChunk(converter, cursor, chunk + extra, encoding.cut, retryEof, writer);
                if (!consumed) {
                    output.remove();
                    return std::unexpected(consumed.error());
                }
            }
            if (*consumed == 0) {
                auto marked = writer.write(kReplacement, 3);
                if (!marked) {
                    output.remove();
                    return std::unexpected(marked.error());
                }
                converter.reset();
                cursor += 1;
                continue;
            }
        }
        cursor += *consumed;
    }
    auto finished = writer.finish();
    if (!finished) {
        output.remove();
        return std::unexpected(finished.error());
    }
    return commitPartial(output, partial, destination);
}

Result<void> normalizeNewlinesToCacheFile(const char* data,
                                           quint64 size,
                                           const QString& destination,
                                           const std::atomic<bool>& cancel)
{
    if (data == nullptr || size == 0) {
        return std::unexpected(makeError(QStringLiteral("文件是空的")));
    }
    QDir().mkpath(QFileInfo(destination).absolutePath());
    const QString partial = destination + QStringLiteral(".partial");
    QFile output(partial);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return std::unexpected(makeError(QStringLiteral("无法写入缓存")));
    }
    Utf8NewlineWriter writer(output);
    constexpr quint64 kStep = 1 << 20;
    const char* cursor = data;
    const char* end = data + size;
    while (cursor < end) {
        if (cancel.load()) {
            output.remove();
            return std::unexpected(cancelledError());
        }
        const quint64 chunk = std::min<quint64>(kStep, static_cast<quint64>(end - cursor));
        auto written = writer.write(cursor, static_cast<qsizetype>(chunk));
        if (!written) {
            output.remove();
            return std::unexpected(written.error());
        }
        cursor += chunk;
    }
    auto finished = writer.finish();
    if (!finished) {
        output.remove();
        return std::unexpected(finished.error());
    }
    return commitPartial(output, partial, destination);
}

}  // namespace moyu
