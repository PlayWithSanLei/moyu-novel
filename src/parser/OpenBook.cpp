#include "parser/OpenBook.h"

#include "core/ChapterDetector.h"
#include "core/Fingerprint.h"
#include "core/Paths.h"
#include "parser/EpubParser.h"
#include "parser/MobiParser.h"
#include "parser/PlainParser.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>

namespace moyu {
namespace {

QString suffixOf(const QString& path)
{
    return QFileInfo(path).suffix().toLower();
}

bool isMobiSuffix(const QString& suffix)
{
    return suffix == QLatin1String("mobi") || suffix == QLatin1String("azw") || suffix == QLatin1String("azw3") ||
           suffix == QLatin1String("prc");
}

Result<ParsedBook> parseFormatted(const QString& path,
                                  const QString& suffix,
                                  const QString& cacheDir,
                                  const QString& token,
                                  const std::atomic<bool>& cancel,
                                  const PreviewHandler& preview)
{
    if (suffix == QLatin1String("epub")) {
        return parseEpub(path, cacheDir, token, cancel, preview);
    }
    if (isMobiSuffix(suffix)) {
        return parseMobi(path, cacheDir, token, cancel, preview);
    }
    if (suffix == QLatin1String("fb2")) {
        return parseFb2(path, cacheDir, token, cancel, preview);
    }
    if (suffix == QLatin1String("html") || suffix == QLatin1String("htm")) {
        return parseHtml(path, cacheDir, token, cancel, preview);
    }
    if (suffix == QLatin1String("md") || suffix == QLatin1String("markdown")) {
        return parseMarkdown(path, cacheDir, token, cancel, preview);
    }
    return std::unexpected(makeError(QStringLiteral("不支持的文件格式")));
}

Result<TxtDocument> rememberAndAssemble(ParsedBook parsed,
                                        Database* database,
                                        quint32 chapterHint,
                                        quint32 byteHint,
                                        bool cacheHit,
                                        OpenTimings* timings,
                                        QElapsedTimer& total)
{
    if (!cacheHit && database != nullptr && database->isOpen()) {
        auto id = database->upsertBook(parsed.book);
        if (!id) {
            return std::unexpected(id.error());
        }
        parsed.book.id = *id;
        auto replaced = database->replaceChapters(*id, parsed.chapters);
        if (!replaced) {
            return std::unexpected(replaced.error());
        }
    }
    return assembleDocument(std::move(parsed), chapterHint, byteHint, cacheHit, timings, total);
}

}  // namespace

bool isSupportedBookPath(const QString& path)
{
    const QString suffix = suffixOf(path);
    return suffix == QLatin1String("txt") || suffix == QLatin1String("epub") || isMobiSuffix(suffix) ||
           suffix == QLatin1String("fb2") || suffix == QLatin1String("html") || suffix == QLatin1String("htm") ||
           suffix == QLatin1String("md") || suffix == QLatin1String("markdown");
}

QString supportedBookFilter()
{
    return QStringLiteral("书籍 (*.txt *.epub *.mobi *.azw *.azw3 *.prc *.fb2 *.html *.htm *.md *.markdown)");
}

Result<TxtDocument> openBookFile(const QString& path,
                                 const QString& cacheDir,
                                 Database* database,
                                 quint32 chapterHint,
                                 quint32 byteHint,
                                 const std::atomic<bool>& cancel,
                                 const PreviewHandler& preview,
                                 OpenTimings* timings,
                                 const QString& chapterPattern)
{
    if (suffixOf(path) == QLatin1String("txt")) {
        return openTxtFile(path, cacheDir, database, chapterHint, byteHint, cancel, preview, timings, chapterPattern);
    }
    if (!isSupportedBookPath(path)) {
        return std::unexpected(makeError(QStringLiteral("不支持的文件格式")));
    }

    QElapsedTimer total;
    total.start();
    if (timings != nullptr) {
        *timings = {};
    }
    const QFileInfo info(path);
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

    if (database != nullptr && database->isOpen()) {
        auto cached = database->findByFingerprint(token);
        if (!cached) {
            return std::unexpected(cached.error());
        }
        if (cached->has_value() && chapterRangesValid((*cached)->chapters, (*cached)->book.totalBytes)) {
            const StoredBook& stored = **cached;
            const QString cachePath = utf8CachePath(cacheDir, token);
            const QFileInfo cacheInfo(cachePath);
            // 原文件是 EPUB 或 MOBI，不能按 TXT 的 UTF-8 直通去 mmap。缓存大小对得上才复用。
            if (cacheInfo.exists() && cacheInfo.size() >= 0 &&
                static_cast<quint64>(cacheInfo.size()) == stored.book.totalBytes) {
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
                parsed.bodyPath = cachePath;
                parsed.bodyOffset = 0;
                return assembleDocument(std::move(parsed), chapterHint, byteHint, true, timings, total);
            }
        }
    }

    if (cancel.load()) {
        return std::unexpected(cancelledError());
    }
    auto parsed = parseFormatted(path, suffixOf(path), cacheDir, token, cancel, preview);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return rememberAndAssemble(std::move(*parsed), database, chapterHint, byteHint, false, timings, total);
}

}  // namespace moyu
