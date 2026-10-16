#include "parser/TxtParser.h"

#include "core/ChapterDetector.h"
#include "core/Encoding.h"
#include "core/Fingerprint.h"
#include "core/MappedFile.h"
#include "core/Paths.h"
#include "core/Utf8.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace moyu {
namespace {

constexpr int kPreviewCodePoints = 8000;
constexpr quint64 kTranscodeChunk = 1 << 20;

bool isTxtPath(const QString& path)
{
    return path.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive);
}

quint32 snapOffset(const char* bytes, quint64 length, quint32 offset)
{
    if (length == 0 || offset == 0) {
        return 0;
    }
    if (static_cast<quint64>(offset) >= length) {
        return static_cast<quint32>(std::min<quint64>(length, std::numeric_limits<quint32>::max()));
    }
    quint64 cursor = 0;
    while (cursor < offset && cursor < length) {
        const int consumed = utf8SequenceLength(bytes + cursor, bytes + length);
        if (consumed <= 0) {
            break;
        }
        if (cursor + static_cast<quint64>(consumed) > offset) {
            break;
        }
        cursor += static_cast<quint64>(consumed);
    }
    return static_cast<quint32>(cursor);
}

ChapterContent previewSlice(const char* body, quint64 bodySize, quint64 origin, quint64* sliceStart)
{
    if (origin > bodySize) {
        origin = 0;
    }
    quint64 start = origin;
    const quint64 backLimit = origin > 4096 ? origin - 4096 : 0;
    while (start > backLimit && body[start - 1] != '\n') {
        --start;
    }
    const char* from = body + start;
    const char* to = from;
    int points = 0;
    while (to < body + bodySize && points < kPreviewCodePoints) {
        const int consumed = utf8SequenceLength(to, body + bodySize);
        if (consumed <= 0) {
            break;
        }
        to += consumed;
        ++points;
    }
    ChapterIndex chapter;
    chapter.title = QStringLiteral("正在建立目录…");
    chapter.byteStart = start;
    chapter.byteEnd = static_cast<quint64>(to - body);
    if (chapter.byteEnd <= chapter.byteStart && bodySize > start) {
        chapter.byteEnd = std::min(bodySize, start + 1);
    }
    if (sliceStart != nullptr) {
        *sliceStart = chapter.byteStart;
    }
    return makeChapterContent(chapter, body, bodySize);
}

quint32 clampedOffset(const ChapterIndex& chapter, quint32 hint)
{
    const quint64 length = chapter.byteEnd - chapter.byteStart;
    if (static_cast<quint64>(hint) > length) {
        return static_cast<quint32>(std::min<quint64>(length, std::numeric_limits<quint32>::max()));
    }
    return hint;
}

Result<ParsedBook> finishParsed(Book book,
                                 std::vector<ChapterIndex> chapters,
                                 const QString& bodyPath,
                                 quint64 bodyOffset)
{
    if (!chapterRangesValid(chapters, book.totalBytes)) {
        return std::unexpected(makeError(QStringLiteral("章节范围不连续")));
    }
    ParsedBook parsed;
    parsed.book = std::move(book);
    parsed.chapters = std::move(chapters);
    parsed.bodyPath = bodyPath;
    parsed.bodyOffset = bodyOffset;
    return parsed;
}

class TxtBuilder {
public:
    TxtBuilder(QString cacheDir, const std::atomic<bool>& cancel)
        : m_cacheDir(std::move(cacheDir))
        , m_cancel(cancel)
    {
    }

    Result<ParsedBook> build(const QString& path, const PreviewHandler& preview, OpenTimings* timings)
    {
        QElapsedTimer step;
        step.start();
        auto mapped = MappedFile::open(path);
        if (!mapped) {
            return std::unexpected(mapped.error());
        }
        if (m_cancel.load()) {
            return std::unexpected(cancelledError());
        }
        auto detected = detectEncoding(mapped->data(), mapped->size());
        if (!detected) {
            return std::unexpected(detected.error());
        }
        if (detected->bodyOffset > mapped->size()) {
            return std::unexpected(makeError(QStringLiteral("文件头损坏")));
        }
        const quint64 sourceBody = mapped->size() - detected->bodyOffset;
        if (sourceBody == 0) {
            return std::unexpected(makeError(QStringLiteral("文件是空的")));
        }

        QString bodyPath = path;
        quint64 bodyOffset = detected->bodyOffset;
        const char* body = mapped->data() + bodyOffset;
        quint64 bodySize = sourceBody;
        MappedFile cacheMap;
        const bool needsNewlineNormalize =
            detected->passthroughUtf8() && std::memchr(body, '\r', static_cast<std::size_t>(sourceBody)) != nullptr;
        // 缓存名是 <指纹>.utf8。指纹变了就对不上这个文件。fsync 之后才 rename，存在就是完整的。
        const auto reuseCache = [&](const QString& cachePath) -> bool {
            const QFileInfo info(cachePath);
            if (m_token.isEmpty() || !info.exists() || !info.isFile() || info.size() <= 0) {
                return false;
            }
            if (info.fileName() != m_token + QStringLiteral(".utf8")) {
                return false;
            }
            auto opened = MappedFile::open(cachePath);
            if (!opened) {
                return false;
            }
            cacheMap = std::move(*opened);
            bodyPath = cachePath;
            bodyOffset = 0;
            body = cacheMap.data();
            bodySize = cacheMap.size();
            return true;
        };
        if (!detected->passthroughUtf8()) {
            const QString cachePath = utf8CachePath(m_cacheDir, m_token);
            if (!reuseCache(cachePath)) {
                auto transcoded = transcodeToCacheFile(body,
                                                        sourceBody,
                                                        *detected,
                                                        cachePath,
                                                        kTranscodeChunk,
                                                        m_cancel);
                if (!transcoded) {
                    return std::unexpected(transcoded.error());
                }
                auto opened = MappedFile::open(cachePath);
                if (!opened) {
                    return std::unexpected(opened.error());
                }
                cacheMap = std::move(*opened);
                bodyPath = cachePath;
                bodyOffset = 0;
                body = cacheMap.data();
                bodySize = cacheMap.size();
            }
        } else if (needsNewlineNormalize) {
            const QString cachePath = utf8CachePath(m_cacheDir, m_token);
            if (!reuseCache(cachePath)) {
                auto normalized = normalizeNewlinesToCacheFile(body, sourceBody, cachePath, m_cancel);
                if (!normalized) {
                    return std::unexpected(normalized.error());
                }
                auto opened = MappedFile::open(cachePath);
                if (!opened) {
                    return std::unexpected(opened.error());
                }
                cacheMap = std::move(*opened);
                bodyPath = cachePath;
                bodyOffset = 0;
                body = cacheMap.data();
                bodySize = cacheMap.size();
            }
        }
        if (timings != nullptr) {
            timings->prepareMs = static_cast<double>(step.nsecsElapsed()) / 1e6;
        }
        if (m_cancel.load()) {
            return std::unexpected(cancelledError());
        }
        if (preview) {
            quint64 sliceStart = 0;
            ChapterContent slice = previewSlice(body, bodySize, m_previewOrigin, &sliceStart);
            preview(std::move(slice), sliceStart, bodySize);
        }

        step.restart();
        auto chapters = detectChapters(body, bodySize, &m_cancel, m_chapterPattern);
        // 设置里的正则写坏了也不能让书打不开。打开时退回内置规则，重新生成目录时再把错误告诉用户。
        if (!chapters && !m_chapterPattern.isEmpty() && chapters.error().message == QStringLiteral("章节正则无效")) {
            chapters = detectChapters(body, bodySize, &m_cancel);
        }
        if (!chapters) {
            return std::unexpected(chapters.error());
        }
        if (timings != nullptr) {
            timings->scanMs = static_cast<double>(step.nsecsElapsed()) / 1e6;
        }

        Book book;
        book.filePath = path;
        book.fingerprint = m_token;
        book.title = QFileInfo(path).completeBaseName();
        book.format = BookFormat::Txt;
        book.sourceEncoding = detected->canonical;
        book.totalBytes = bodySize;
        book.lastReadAt = QDateTime::currentMSecsSinceEpoch();
        return finishParsed(std::move(book), std::move(*chapters), bodyPath, bodyOffset);
    }

    void setToken(QString token) { m_token = std::move(token); }
    void setPreviewOrigin(quint64 origin) { m_previewOrigin = origin; }
    void setChapterPattern(QString pattern) { m_chapterPattern = std::move(pattern); }

private:
    QString m_cacheDir;
    const std::atomic<bool>& m_cancel;
    QString m_token;
    QString m_chapterPattern;
    quint64 m_previewOrigin = 0;
};

}  // namespace

Result<TxtDocument> assembleDocument(ParsedBook parsed,
                                     quint32 chapterHint,
                                     quint32 byteHint,
                                     bool cacheHit,
                                     OpenTimings* timings,
                                     QElapsedTimer& total)
{
    if (parsed.chapters.empty()) {
        return std::unexpected(makeError(QStringLiteral("没有章节")));
    }
    // 提示超出新文件时落到最后一章，不退回第 0 章。字节偏移夹进这一章，章尾本身是合法位置。
    quint32 index = chapterHint < parsed.chapters.size()
                         ? chapterHint
                         : static_cast<quint32>(parsed.chapters.size() - 1);
    QElapsedTimer step;
    step.start();
    auto content = readMappedChapter(parsed.bodyPath, parsed.bodyOffset, parsed.chapters[index]);
    if (!content) {
        return std::unexpected(content.error());
    }
    if (timings != nullptr) {
        timings->chapterMs = static_cast<double>(step.nsecsElapsed()) / 1e6;
        timings->totalMs = static_cast<double>(total.nsecsElapsed()) / 1e6;
        timings->cacheHit = cacheHit;
    }
    TxtDocument document;
    document.chapter = std::move(*content);
    document.chapterIndex = index;
    document.byteOffset = clampedOffset(parsed.chapters[index], byteHint);
    document.cacheHit = cacheHit;
    document.parsed = std::move(parsed);
    const auto length = document.parsed.chapters[index].byteEnd - document.parsed.chapters[index].byteStart;
    document.byteOffset = snapOffset(document.chapter.utf8.constData(), length, document.byteOffset);
    return document;
}

TxtParser::TxtParser(QString cacheDir)
    : m_cacheDir(std::move(cacheDir))
{
}

Result<ParsedBook> TxtParser::parseIndex(const QString& path, const std::atomic<bool>& cancel)
{
    if (!isTxtPath(path)) {
        return std::unexpected(makeError(QStringLiteral("目前只支持 TXT")));
    }
    auto fingerprint = fingerprintOf(path);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    TxtBuilder builder(m_cacheDir, cancel);
    builder.setToken(fingerprint->token());
    return builder.build(path, {}, nullptr);
}

Result<ChapterContent> TxtParser::readChapter(const ParsedBook& book, quint32 index)
{
    if (index >= book.chapters.size()) {
        return std::unexpected(makeError(QStringLiteral("章节不存在")));
    }
    return readMappedChapter(book.bodyPath, book.bodyOffset, book.chapters[index]);
}

Result<ChapterContent> readMappedChapter(const QString& bodyPath,
                                          quint64 bodyOffset,
                                          const ChapterIndex& chapter)
{
    auto mapped = MappedFile::open(bodyPath);
    if (!mapped) {
        return std::unexpected(mapped.error());
    }
    if (bodyOffset > mapped->size()) {
        return std::unexpected(makeError(QStringLiteral("缓存文件损坏")));
    }
    const quint64 bodySize = mapped->size() - bodyOffset;
    if (chapter.byteStart > chapter.byteEnd || chapter.byteEnd > bodySize) {
        return std::unexpected(makeError(QStringLiteral("章节偏移超出文件")));
    }
    return makeChapterContent(chapter, mapped->data() + bodyOffset, bodySize);
}

Result<TxtDocument> openTxtFile(const QString& path,
                                 const QString& cacheDir,
                                 Database* database,
                                 quint32 chapterHint,
                                 quint32 byteHint,
                                 const std::atomic<bool>& cancel,
                                 const PreviewHandler& preview,
                                 OpenTimings* timings,
                                 const QString& chapterPattern)
{
    QElapsedTimer total;
    total.start();
    if (timings != nullptr) {
        *timings = {};
    }
    if (!isTxtPath(path)) {
        return std::unexpected(makeError(QStringLiteral("目前只支持 TXT")));
    }
    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return std::unexpected(makeError(QStringLiteral("找不到文件")));
    }
    if (info.size() == 0) {
        return std::unexpected(makeError(QStringLiteral("文件是空的")));
    }
    if (cancel.load()) {
        return std::unexpected(cancelledError());
    }

    QElapsedTimer step;
    step.start();
    auto fingerprint = fingerprintOf(path);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    if (timings != nullptr) {
        timings->fingerprintMs = static_cast<double>(step.nsecsElapsed()) / 1e6;
    }
    const QString token = fingerprint->token();
    quint64 previewOrigin = 0;

    if (database != nullptr && database->isOpen()) {
        auto cached = database->findByFingerprint(token);
        if (!cached) {
            return std::unexpected(cached.error());
        }
        if (cached->has_value() && chapterRangesValid((*cached)->chapters, (*cached)->book.totalBytes)) {
            const StoredBook& stored = **cached;
            if (chapterHint < stored.chapters.size()) {
                const ChapterIndex& hinted = stored.chapters[chapterHint];
                const quint64 position = hinted.byteStart + byteHint;
                if (position <= stored.book.totalBytes) {
                    previewOrigin = position;
                }
            }
            QString bodyPath;
            quint64 bodyOffset = 0;
            bool usable = false;
            if (stored.book.sourceEncoding == QLatin1String("UTF-8")) {
                auto mapped = MappedFile::open(path);
                if (mapped) {
                    // 这本书打开时已经验过 UTF-8。指纹没变就不要再扫一遍。
                    auto detected = detectEncoding(mapped->data(), mapped->size(), false);
                    if (detected && detected->passthroughUtf8() &&
                        mapped->size() - detected->bodyOffset == stored.book.totalBytes) {
                        bodyPath = path;
                        bodyOffset = detected->bodyOffset;
                        usable = true;
                    } else {
                        const QString cachePath = utf8CachePath(cacheDir, token);
                        const QFileInfo cacheInfo(cachePath);
                        if (cacheInfo.exists() && static_cast<quint64>(cacheInfo.size()) == stored.book.totalBytes) {
                            bodyPath = cachePath;
                            usable = true;
                        }
                    }
                }
            } else {
                const QString cachePath = utf8CachePath(cacheDir, token);
                const QFileInfo cacheInfo(cachePath);
                if (cacheInfo.exists() && cacheInfo.size() >= 0 &&
                    static_cast<quint64>(cacheInfo.size()) == stored.book.totalBytes) {
                    bodyPath = cachePath;
                    usable = true;
                }
            }
            if (usable) {
                Book book = stored.book;
                book.filePath = path;
                book.lastReadAt = QDateTime::currentMSecsSinceEpoch();
                auto id = database->upsertBook(book);
                if (!id) {
                    return std::unexpected(id.error());
                }
                book.id = *id;
                ParsedBook parsed;
                parsed.book = std::move(book);
                parsed.chapters = stored.chapters;
                parsed.bodyPath = bodyPath;
                parsed.bodyOffset = bodyOffset;
                return assembleDocument(std::move(parsed), chapterHint, byteHint, true, timings, total);
            }
        }
    }

    if (cancel.load()) {
        return std::unexpected(cancelledError());
    }
    TxtBuilder builder(cacheDir, cancel);
    builder.setToken(token);
    builder.setPreviewOrigin(previewOrigin);
    builder.setChapterPattern(chapterPattern);
    auto parsed = builder.build(path, preview, timings);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (database != nullptr && database->isOpen()) {
        auto id = database->upsertBook(parsed->book);
        if (!id) {
            return std::unexpected(id.error());
        }
        parsed->book.id = *id;
        auto replaced = database->replaceChapters(*id, parsed->chapters);
        if (!replaced) {
            return std::unexpected(replaced.error());
        }
    }
    return assembleDocument(std::move(*parsed), chapterHint, byteHint, false, timings, total);
}

}  // namespace moyu
