#include "parser/PlainParser.h"

#include "core/ChapterDetector.h"
#include "core/Encoding.h"
#include "core/MappedFile.h"
#include "parser/Markup.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <cstring>
#include <limits>
#include <vector>

#include <pugixml.hpp>

namespace moyu {
namespace {

constexpr quint64 kMaxPlainBytes = 64ULL * 1024ULL * 1024ULL;

QString fallbackTitle(const QString& path)
{
    const QString base = QFileInfo(path).completeBaseName();
    return base.isEmpty() ? QStringLiteral("正文") : base;
}

Result<QByteArray> loadUtf8(const QString& path,
                           const QString& cacheDir,
                           const QString& token,
                           const std::atomic<bool>& cancel,
                           bool preferHtmlMeta)
{
    auto mapped = MappedFile::open(path);
    if (!mapped) {
        return std::unexpected(mapped.error());
    }
    if (mapped->size() > kMaxPlainBytes) {
        return std::unexpected(makeError(QStringLiteral("文件太大")));
    }
    Result<DetectedEncoding> detected = std::unexpected(makeError(QString()));
    if (preferHtmlMeta) {
        const QString meta = htmlMetaCharset(mapped->data(), mapped->size());
        if (!meta.isEmpty()) {
            auto declared = encodingFromDeclaredName(meta);
            if (declared) {
                detected = std::move(declared);
            }
        }
    }
    if (!detected) {
        detected = detectEncoding(mapped->data(), mapped->size(), true);
    }
    if (!detected) {
        return std::unexpected(detected.error());
    }
    if (detected->bodyOffset > mapped->size()) {
        return std::unexpected(makeError(QStringLiteral("文件损坏")));
    }
    const char* body = mapped->data() + detected->bodyOffset;
    const quint64 bodySize = mapped->size() - detected->bodyOffset;
    if (detected->passthroughUtf8()) {
        return QByteArray(body, static_cast<qsizetype>(bodySize));
    }
    if (!QDir().mkpath(cacheDir)) {
        return std::unexpected(makeError(QStringLiteral("无法创建缓存目录")));
    }
    const QString temp = cacheDir + QLatin1Char('/') + token + QStringLiteral(".src.utf8");
    auto wrote = transcodeToCacheFile(mapped->data(), mapped->size(), *detected, temp, 1ULL << 20, cancel);
    if (!wrote) {
        QFile::remove(temp);
        return std::unexpected(wrote.error());
    }
    auto cached = MappedFile::open(temp);
    if (!cached) {
        QFile::remove(temp);
        return std::unexpected(cached.error());
    }
    QByteArray utf8(cached->data(), static_cast<qsizetype>(cached->size()));
    cached = MappedFile{};
    QFile::remove(temp);
    return utf8;
}

Book makeBook(const QString& path, const QString& token, BookFormat format, const QString& title, const QString& author)
{
    Book book;
    book.filePath = path;
    book.fingerprint = token;
    book.title = title.isEmpty() ? fallbackTitle(path) : title;
    book.author = author;
    book.format = format;
    book.lastReadAt = QDateTime::currentMSecsSinceEpoch();
    return book;
}

void previewFirst(const std::vector<TextChapter>& chapters, const PreviewHandler& preview)
{
    if (!preview || chapters.empty()) {
        return;
    }
    ChapterIndex meta;
    meta.title = chapters.front().title;
    meta.byteEnd = static_cast<quint64>(chapters.front().utf8.size());
    preview(makeChapterContent(meta, chapters.front().utf8.constData(), meta.byteEnd), 0, meta.byteEnd);
}

std::vector<TextChapter> markdownChapters(const QString& text, const QString& bookTitle)
{
    QString marked;
    const QStringList lines = text.split(QLatin1Char('\n'));
    QRegularExpression heading(QStringLiteral("^#{1,6}[ \\t]+(\\S.*)$"));
    bool fenced = false;
    QString fenceMark;
    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        if (line.startsWith(QLatin1String("```")) || line.startsWith(QLatin1String("~~~"))) {
            const QString mark = line.startsWith(QLatin1String("```")) ? QStringLiteral("```") : QStringLiteral("~~~");
            if (!fenced) {
                fenced = true;
                fenceMark = mark;
            } else if (line.startsWith(fenceMark)) {
                fenced = false;
            }
            marked += line;
            marked += QLatin1Char('\n');
            continue;
        }
        const QRegularExpressionMatch match = heading.match(line);
        if (!fenced && match.hasMatch()) {
            marked += QChar(static_cast<char16_t>(static_cast<unsigned char>(kHeadingMark)));
            marked += match.captured(1).trimmed();
            marked += QLatin1Char('\n');
            continue;
        }
        marked += line;
        marked += QLatin1Char('\n');
    }
    return chaptersFromMarkedText(marked, bookTitle);
}

QString localOf(const pugi::xml_node& node)
{
    const char* full = node.name();
    const char* colon = std::strchr(full, ':');
    return QString::fromUtf8(colon != nullptr ? colon + 1 : full);
}

bool localEquals(const pugi::xml_node& node, const char* name)
{
    const char* full = node.name();
    const char* colon = std::strchr(full, ':');
    return std::strcmp(colon != nullptr ? colon + 1 : full, name) == 0;
}

// 内联标签里的字也要留下。<p>他说<emphasis>不</emphasis>行</p> 是「他说不行」。
QString descendantText(const pugi::xml_node& root)
{
    QString text;
    struct Frame {
        pugi::xml_node node;
        pugi::xml_node child;
        bool entered = false;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{root, {}, false});
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (!frame.entered) {
            frame.entered = true;
            const pugi::xml_node_type type = frame.node.type();
            if (type == pugi::node_pcdata || type == pugi::node_cdata) {
                text += QString::fromUtf8(frame.node.value());
                stack.pop_back();
                continue;
            }
            frame.child = frame.node.first_child();
        }
        if (stack.empty()) {
            break;
        }
        Frame& current = stack.back();
        if (!current.child) {
            stack.pop_back();
            continue;
        }
        const pugi::xml_node child = current.child;
        current.child = current.child.next_sibling();
        stack.push_back(Frame{child, {}, false});
    }
    return text;
}

// 显式栈。节里再套诗、引文时不再按深度递归。
QString nodeText(const pugi::xml_node& root)
{
    QString text;
    struct Frame {
        pugi::xml_node node;
        pugi::xml_node child;
        bool entered = false;
        bool skipSectionBits = false;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{root, {}, false, false});
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (!frame.entered) {
            frame.entered = true;
            const QString name = localOf(frame.node);
            if (name == QLatin1String("p") || name == QLatin1String("v") || name == QLatin1String("subtitle") ||
                name == QLatin1String("text-author")) {
                text += descendantText(frame.node).trimmed();
                text += QLatin1Char('\n');
                stack.pop_back();
                continue;
            }
            if (name == QLatin1String("empty-line")) {
                text += QLatin1Char('\n');
                stack.pop_back();
                continue;
            }
            const bool container = name == QLatin1String("section") || name == QLatin1String("body") ||
                                   name == QLatin1String("title") || name == QLatin1String("poem") ||
                                   name == QLatin1String("stanza") || name == QLatin1String("cite") ||
                                   name == QLatin1String("annotation");
            if (!container) {
                stack.pop_back();
                continue;
            }
            frame.skipSectionBits = name == QLatin1String("section");
            frame.child = frame.node.first_child();
        }
        if (stack.empty()) {
            break;
        }
        Frame& current = stack.back();
        if (!current.child) {
            stack.pop_back();
            continue;
        }
        const pugi::xml_node child = current.child;
        const bool skipSectionBits = current.skipSectionBits;
        current.child = current.child.next_sibling();
        // 节标题单独成章名，正文不再重复那一行。嵌套的 section 另开一章。
        if (skipSectionBits && (localEquals(child, "section") || localEquals(child, "title"))) {
            continue;
        }
        stack.push_back(Frame{child, {}, false, false});
    }
    return text;
}

QString sectionTitle(const pugi::xml_node& section)
{
    for (pugi::xml_node child = section.first_child(); child != nullptr; child = child.next_sibling()) {
        if (!localEquals(child, "title")) {
            continue;
        }
        return descendantText(child).simplified();
    }
    return {};
}

void appendSection(const pugi::xml_node& section, bool nested, std::vector<TextChapter>& chapters)
{
    const QString title = sectionTitle(section);
    const QString body = nodeText(section).trimmed();
    if (title.isEmpty() && body.isEmpty()) {
        return;
    }
    TextChapter chapter;
    chapter.title = title.isEmpty() ? QStringLiteral("正文") : title;
    chapter.level = nested ? 2 : 1;
    const QString text = title.isEmpty() ? body : title + QLatin1Char('\n') + body;
    chapter.utf8 = text.toUtf8();
    chapters.push_back(std::move(chapter));
}

void collectSections(const pugi::xml_node& root, bool nested, std::vector<TextChapter>& chapters)
{
    struct Job {
        pugi::xml_node node;
        bool nested = false;
        bool expand = false;
    };
    std::vector<Job> stack;
    stack.push_back(Job{root, nested, true});
    while (!stack.empty()) {
        const Job job = stack.back();
        stack.pop_back();
        if (!job.expand) {
            appendSection(job.node, job.nested, chapters);
            continue;
        }
        std::vector<pugi::xml_node> sections;
        for (pugi::xml_node child = job.node.first_child(); child != nullptr; child = child.next_sibling()) {
            if (localEquals(child, "section")) {
                sections.push_back(child);
            }
        }
        for (std::size_t i = sections.size(); i > 0; --i) {
            stack.push_back(Job{sections[i - 1], true, true});
            stack.push_back(Job{sections[i - 1], job.nested, false});
        }
    }
}

QString directChildText(const pugi::xml_node& node, const char* wanted)
{
    for (pugi::xml_node child = node.first_child(); child != nullptr; child = child.next_sibling()) {
        if (localEquals(child, wanted)) {
            return descendantText(child).trimmed();
        }
    }
    return {};
}

}  // namespace

Result<ParsedBook> parseHtml(const QString& path,
                             const QString& cacheDir,
                             const QString& token,
                             const std::atomic<bool>& cancel,
                             const PreviewHandler& preview)
{
    auto utf8 = loadUtf8(path, cacheDir, token, cancel, true);
    if (!utf8) {
        return std::unexpected(utf8.error());
    }
    QString plain;
    auto xhtml = xhtmlToPlain(utf8->constData(), static_cast<quint64>(utf8->size()));
    if (xhtml) {
        plain = std::move(xhtml->text);
    } else {
        plain = htmlToPlainLenient(utf8->constData(), static_cast<quint64>(utf8->size()), &cancel).text;
        if (cancel.load()) {
            return std::unexpected(cancelledError());
        }
    }
    const std::vector<TextChapter> chapters = chaptersFromMarkedText(plain, fallbackTitle(path));
    previewFirst(chapters, preview);
    return writeBodyCache(makeBook(path, token, BookFormat::Html, fallbackTitle(path), {}), chapters, cacheDir, token, cancel);
}

Result<ParsedBook> parseMarkdown(const QString& path,
                                 const QString& cacheDir,
                                 const QString& token,
                                 const std::atomic<bool>& cancel,
                                 const PreviewHandler& preview)
{
    auto utf8 = loadUtf8(path, cacheDir, token, cancel, false);
    if (!utf8) {
        return std::unexpected(utf8.error());
    }
    const QString text = QString::fromUtf8(utf8->constData(), static_cast<qsizetype>(utf8->size()));
    const std::vector<TextChapter> chapters = markdownChapters(text, fallbackTitle(path));
    previewFirst(chapters, preview);
    return writeBodyCache(makeBook(path, token, BookFormat::Markdown, fallbackTitle(path), {}),
                          chapters,
                          cacheDir,
                          token,
                          cancel);
}

Result<ParsedBook> parseFb2(const QString& path,
                            const QString& cacheDir,
                            const QString& token,
                            const std::atomic<bool>& cancel,
                            const PreviewHandler& preview)
{
    auto utf8 = loadUtf8(path, cacheDir, token, cancel, false);
    if (!utf8) {
        return std::unexpected(utf8.error());
    }
    if (markupDeeperThan(utf8->constData(), static_cast<quint64>(utf8->size()), kMaxMarkupDepth)) {
        return std::unexpected(makeError(QStringLiteral("文件结构异常")));
    }
    pugi::xml_document document;
    if (!document.load_buffer(utf8->constData(), static_cast<std::size_t>(utf8->size()))) {
        return std::unexpected(makeError(QStringLiteral("FB2 无法解析")));
    }
    const pugi::xml_node root = document.document_element();
    pugi::xml_node description;
    pugi::xml_node body;
    for (pugi::xml_node child = root.first_child(); child != nullptr; child = child.next_sibling()) {
        const char* full = child.name();
        const char* colon = std::strchr(full, ':');
        const char* local = colon != nullptr ? colon + 1 : full;
        if (std::strcmp(local, "description") == 0) {
            description = child;
        } else if (std::strcmp(local, "body") == 0 && !body) {
            body = child;
        }
    }
    QString title = fallbackTitle(path);
    QString author;
    for (pugi::xml_node child = description.first_child(); child != nullptr; child = child.next_sibling()) {
        const char* full = child.name();
        const char* colon = std::strchr(full, ':');
        if (std::strcmp(colon != nullptr ? colon + 1 : full, "title-info") != 0) {
            continue;
        }
        title = directChildText(child, "book-title");
        if (title.isEmpty()) {
            title = fallbackTitle(path);
        }
        for (pugi::xml_node authorNode = child.first_child(); authorNode != nullptr; authorNode = authorNode.next_sibling()) {
            const char* authorName = authorNode.name();
            const char* authorColon = std::strchr(authorName, ':');
            if (std::strcmp(authorColon != nullptr ? authorColon + 1 : authorName, "author") != 0) {
                continue;
            }
            const QString first = directChildText(authorNode, "first-name");
            const QString last = directChildText(authorNode, "last-name");
            if (first.isEmpty()) {
                author = last;
            } else if (last.isEmpty()) {
                author = first;
            } else if (first.at(0).unicode() > 127 && last.at(0).unicode() > 127) {
                author = first + last;
            } else {
                author = first + QLatin1Char(' ') + last;
            }
            break;
        }
        break;
    }
    std::vector<TextChapter> chapters;
    collectSections(body, false, chapters);
    if (chapters.empty()) {
        const QString text = nodeText(body).trimmed();
        if (!text.isEmpty()) {
            TextChapter chapter;
            chapter.title = title;
            chapter.utf8 = text.toUtf8();
            chapters.push_back(std::move(chapter));
        }
    }
    previewFirst(chapters, preview);
    return writeBodyCache(makeBook(path, token, BookFormat::Fb2, title, author), chapters, cacheDir, token, cancel);
}

}  // namespace moyu
