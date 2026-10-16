#include "parser/BodyCache.h"

#include "core/ChapterDetector.h"
#include "core/Paths.h"
#include "core/Utf8.h"

#include <QDir>
#include <QFile>

#include <fcntl.h>
#include <limits>
#include <unistd.h>

namespace moyu {
namespace {

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

bool hasVisibleText(const QByteArray& utf8)
{
    for (char byte : utf8) {
        const auto value = static_cast<unsigned char>(byte);
        if (value != ' ' && value != '\n' && value != '\r' && value != '\t' && value != static_cast<unsigned char>(kHeadingMark)) {
            return true;
        }
    }
    return false;
}

int countPoints(const char* begin, const char* end, int limit)
{
    int points = 0;
    const char* cursor = begin;
    while (cursor < end && points < limit) {
        const int consumed = utf8SequenceLength(cursor, end);
        if (consumed <= 0) {
            break;
        }
        cursor += consumed;
        ++points;
    }
    return points;
}

std::vector<QByteArray> splitLong(const QByteArray& text)
{
    const char* begin = text.constData();
    const char* end = begin + text.size();
    if (countPoints(begin, end, kMaxChapterCodePoints + 1) <= kMaxChapterCodePoints) {
        return {text};
    }
    std::vector<QByteArray> pieces;
    const char* cursor = begin;
    while (cursor < end) {
        const char* limit = cursor;
        int points = 0;
        const char* lastBreak = nullptr;
        while (limit < end && points < kSyntheticChapterCodePoints) {
            if (*limit == '\n') {
                lastBreak = limit + 1;
            }
            const int consumed = utf8SequenceLength(limit, end);
            if (consumed <= 0) {
                limit = end;
                break;
            }
            limit += consumed;
            ++points;
        }
        const char* pieceEnd = limit;
        if (lastBreak != nullptr && lastBreak > cursor && points >= kSyntheticChapterCodePoints) {
            pieceEnd = lastBreak;
        }
        if (pieceEnd <= cursor) {
            pieceEnd = end;
        }
        pieces.emplace_back(cursor, static_cast<qsizetype>(pieceEnd - cursor));
        cursor = pieceEnd;
    }
    return pieces;
}

QByteArray withTrailingNewline(QByteArray bytes)
{
    if (bytes.isEmpty() || bytes.back() != '\n') {
        bytes.append('\n');
    }
    return bytes;
}

}  // namespace

Result<ParsedBook> writeBodyCache(Book book,
                                  const std::vector<TextChapter>& chapters,
                                  const QString& cacheDir,
                                  const QString& token,
                                  const std::atomic<bool>& cancel)
{
    if (!QDir().mkpath(cacheDir)) {
        return std::unexpected(makeError(QStringLiteral("无法创建缓存目录")));
    }
    const QString destination = utf8CachePath(cacheDir, token);
    const QString partial = destination + QStringLiteral(".partial");
    QFile output(partial);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return std::unexpected(makeError(QStringLiteral("无法写入缓存")));
    }

    std::vector<ChapterIndex> indexes;
    quint64 cursor = 0;
    for (const TextChapter& chapter : chapters) {
        if (cancel.load()) {
            output.remove();
            return std::unexpected(cancelledError());
        }
        if (!hasVisibleText(chapter.utf8)) {
            continue;
        }
        const std::vector<QByteArray> pieces = splitLong(chapter.utf8);
        int pieceNo = 1;
        for (const QByteArray& piece : pieces) {
            QByteArray bytes = withTrailingNewline(piece);
            if (cursor > std::numeric_limits<quint64>::max() - static_cast<quint64>(bytes.size())) {
                output.remove();
                return std::unexpected(makeError(QStringLiteral("正文太长")));
            }
            if (output.write(bytes) != bytes.size()) {
                output.remove();
                return std::unexpected(makeError(QStringLiteral("写入缓存失败")));
            }
            ChapterIndex index;
            index.title = chapter.title.isEmpty() ? QStringLiteral("正文") : chapter.title;
            index.level = chapter.level == 0 ? 2 : chapter.level;
            index.byteStart = cursor;
            cursor += static_cast<quint64>(bytes.size());
            index.byteEnd = cursor;
            if (pieceNo > 1) {
                index.title += QStringLiteral(" · ") + QString::number(pieceNo);
                index.synthetic = true;
                if (index.level == 1) {
                    index.level = 2;
                }
            }
            indexes.push_back(std::move(index));
            ++pieceNo;
        }
    }
    if (indexes.empty() || cursor == 0) {
        output.remove();
        return std::unexpected(makeError(QStringLiteral("没有可阅读的章节")));
    }
    if (indexes.size() > static_cast<std::size_t>(std::numeric_limits<quint32>::max())) {
        output.remove();
        return std::unexpected(makeError(QStringLiteral("章节太多")));
    }
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        indexes[i].index = static_cast<quint32>(i);
    }
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

    book.totalBytes = cursor;
    book.sourceEncoding = QStringLiteral("UTF-8");
    if (!chapterRangesValid(indexes, book.totalBytes)) {
        return std::unexpected(makeError(QStringLiteral("章节范围不连续")));
    }
    ParsedBook parsed;
    parsed.book = std::move(book);
    parsed.chapters = std::move(indexes);
    parsed.bodyPath = destination;
    parsed.bodyOffset = 0;
    return parsed;
}

}  // namespace moyu
