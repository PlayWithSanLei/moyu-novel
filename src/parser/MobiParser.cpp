#include "parser/MobiParser.h"

#include "core/ChapterDetector.h"
#include "parser/Markup.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <cstdlib>
#include <limits>

#include <mobi.h>

namespace moyu {
namespace {

constexpr quint64 kMaxMobiTextBytes = 256ULL * 1024ULL * 1024ULL;
constexpr quint64 kMaxMobiPartBytes = 32ULL * 1024ULL * 1024ULL;
constexpr int kMaxMobiParts = 10000;

struct MobiGuard {
    MOBIData* data = nullptr;
    ~MobiGuard() { mobi_free(data); }
    MobiGuard() = default;
    MobiGuard(const MobiGuard&) = delete;
    MobiGuard& operator=(const MobiGuard&) = delete;
};

struct RawmlGuard {
    MOBIRawml* rawml = nullptr;
    ~RawmlGuard() { mobi_free_rawml(rawml); }
    RawmlGuard() = default;
    RawmlGuard(const RawmlGuard&) = delete;
    RawmlGuard& operator=(const RawmlGuard&) = delete;
};

QString takeOwned(char* text)
{
    if (text == nullptr) {
        return {};
    }
    const QString value = QString::fromUtf8(text).trimmed();
    std::free(text);
    return value;
}

QString mobiFailure(MOBI_RET code)
{
    if (code == MOBI_FILE_ENCRYPTED) {
        return QStringLiteral("这本书有 DRM，不能打开");
    }
    return QStringLiteral("MOBI 无法解析");
}

MOBIData* preferredRecord(MOBIData* data)
{
    // 加载时 use_kf8 为真，混合文件的主记录已经换成 KF8。这里再确认一次。
    if (data != nullptr && mobi_is_hybrid(data) && !mobi_is_kf8(data) && data->next != nullptr && mobi_is_kf8(data->next)) {
        return data->next;
    }
    return data;
}

int countHtmlParts(MOBIPart* head)
{
    int count = 0;
    int guard = 0;
    for (MOBIPart* part = head; part != nullptr; part = part->next) {
        if (++guard > kMaxMobiParts) {
            return -1;
        }
        if (part->type == T_HTML && part->data != nullptr && part->size > 0) {
            ++count;
        }
    }
    return count;
}

Result<int> appendParts(MOBIPart* head,
                        const QString& bookTitle,
                        const std::atomic<bool>& cancel,
                        const PreviewHandler& preview,
                        bool* previewed,
                        std::vector<TextChapter>& chapters)
{
    const int htmlCount = countHtmlParts(head);
    if (htmlCount < 0) {
        return std::unexpected(makeError(QStringLiteral("章节太多")));
    }
    // 只有一个 HTML 片段时，它就是全书，上限跟整本一致。多片段仍各自封顶。
    const quint64 partLimit = htmlCount == 1 ? kMaxMobiTextBytes : kMaxMobiPartBytes;
    int htmlParts = 0;
    int guard = 0;
    for (MOBIPart* part = head; part != nullptr; part = part->next) {
        if (++guard > kMaxMobiParts) {
            return std::unexpected(makeError(QStringLiteral("章节太多")));
        }
        if (cancel.load()) {
            return std::unexpected(cancelledError());
        }
        if (part->type != T_HTML || part->data == nullptr || part->size == 0) {
            continue;
        }
        if (part->size > partLimit || part->size > static_cast<size_t>(std::numeric_limits<qsizetype>::max())) {
            return std::unexpected(makeError(QStringLiteral("文件太大")));
        }
        QString plain;
        auto xhtml = xhtmlToPlain(reinterpret_cast<const char*>(part->data), static_cast<quint64>(part->size));
        if (cancel.load()) {
            return std::unexpected(cancelledError());
        }
        if (xhtml) {
            plain = std::move(xhtml->text);
        } else {
            plain = htmlToPlainLenient(reinterpret_cast<const char*>(part->data), static_cast<quint64>(part->size), &cancel).text;
            if (cancel.load()) {
                return std::unexpected(cancelledError());
            }
        }
        if (plain.trimmed().isEmpty()) {
            continue;
        }
        ++htmlParts;
        QString fallback = bookTitle.isEmpty() ? QStringLiteral("正文") : bookTitle;
        if (htmlParts > 1) {
            fallback += QStringLiteral(" · ") + QString::number(htmlParts);
        }
        std::vector<TextChapter> pieces = chaptersFromMarkedText(plain, fallback);
        for (TextChapter& piece : pieces) {
            if (chapters.size() >= 10000) {
                return std::unexpected(makeError(QStringLiteral("章节太多")));
            }
            if (!*previewed && preview) {
                ChapterIndex meta;
                meta.title = piece.title;
                meta.byteEnd = static_cast<quint64>(piece.utf8.size());
                preview(makeChapterContent(meta, piece.utf8.constData(), meta.byteEnd), 0, meta.byteEnd);
                *previewed = true;
            }
            chapters.push_back(std::move(piece));
        }
    }
    return htmlParts;
}

}  // namespace

Result<ParsedBook> parseMobi(const QString& path,
                             const QString& cacheDir,
                             const QString& token,
                             const std::atomic<bool>& cancel,
                             const PreviewHandler& preview)
{
    if (cancel.load()) {
        return std::unexpected(cancelledError());
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return std::unexpected(makeError(QStringLiteral("找不到文件")));
    }
    if (info.size() <= 0) {
        return std::unexpected(makeError(QStringLiteral("文件是空的")));
    }
    if (static_cast<quint64>(info.size()) > kMaxMobiTextBytes) {
        return std::unexpected(makeError(QStringLiteral("文件太大")));
    }

    MobiGuard mobi;
    mobi.data = mobi_init();
    if (mobi.data == nullptr) {
        return std::unexpected(makeError(QStringLiteral("MOBI 无法解析")));
    }
    // 混合文件优先读 KF8。默认已经是这个选择，调用一次把意图写在打开路径上。
    mobi_parse_kf8(mobi.data);
    const QByteArray encoded = QFile::encodeName(path);
    const MOBI_RET loaded = mobi_load_filename(mobi.data, encoded.constData());
    if (loaded != MOBI_SUCCESS) {
        return std::unexpected(makeError(mobiFailure(loaded)));
    }
    MOBIData* record = preferredRecord(mobi.data);
    if (record == nullptr || record->rh == nullptr) {
        return std::unexpected(makeError(QStringLiteral("MOBI 无法解析")));
    }
    // 加密标志来自记录头。这里只拒绝，不调用 mobi_drm_decrypt。
    if (mobi_is_encrypted(record)) {
        return std::unexpected(makeError(QStringLiteral("这本书有 DRM，不能打开")));
    }
    const size_t textMax = mobi_get_text_maxsize(record);
    if (textMax == MOBI_NOTSET || textMax > kMaxMobiTextBytes || record->rh->text_length > kMaxMobiTextBytes) {
        return std::unexpected(makeError(QStringLiteral("文件太大")));
    }

    RawmlGuard rawml;
    rawml.rawml = mobi_init_rawml(record);
    if (rawml.rawml == nullptr) {
        return std::unexpected(makeError(QStringLiteral("MOBI 无法解析")));
    }
    const MOBI_RET parsed = mobi_parse_rawml(rawml.rawml, record);
    if (parsed != MOBI_SUCCESS) {
        return std::unexpected(makeError(mobiFailure(parsed)));
    }

    const QString filename = info.completeBaseName().isEmpty() ? QStringLiteral("正文") : info.completeBaseName();
    const QString title = [&] {
        const QString meta = takeOwned(mobi_meta_get_title(record));
        return meta.isEmpty() ? filename : meta;
    }();
    const QString author = takeOwned(mobi_meta_get_author(record));

    std::vector<TextChapter> chapters;
    bool previewed = false;
    auto fromMarkup = appendParts(rawml.rawml->markup, title, cancel, preview, &previewed, chapters);
    if (!fromMarkup) {
        return std::unexpected(fromMarkup.error());
    }
    if (*fromMarkup == 0) {
        auto fromFlow = appendParts(rawml.rawml->flow, title, cancel, preview, &previewed, chapters);
        if (!fromFlow) {
            return std::unexpected(fromFlow.error());
        }
    }

    Book book;
    book.filePath = path;
    book.fingerprint = token;
    book.title = title;
    book.author = author;
    book.format = BookFormat::Mobi;
    book.lastReadAt = QDateTime::currentMSecsSinceEpoch();
    return writeBodyCache(std::move(book), chapters, cacheDir, token, cancel);
}

}  // namespace moyu
