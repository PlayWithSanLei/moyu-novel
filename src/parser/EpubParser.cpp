#include "parser/EpubParser.h"

#include "core/ChapterDetector.h"
#include "parser/Markup.h"
#include "parser/ZipArchive.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QHash>
#include <QStringList>
#include <QUrl>

#include <cstring>
#include <utility>
#include <vector>

#include <pugixml.hpp>

namespace moyu {
namespace {

QString localName(const pugi::xml_node& node)
{
    const char* full = node.name();
    const char* colon = std::strchr(full, ':');
    return QString::fromUtf8(colon != nullptr ? colon + 1 : full);
}

pugi::xml_node childByLocal(const pugi::xml_node& node, const char* name)
{
    for (pugi::xml_node child = node.first_child(); child != nullptr; child = child.next_sibling()) {
        if (localName(child) == QLatin1String(name)) {
            return child;
        }
    }
    return {};
}

QString attrText(const pugi::xml_node& node, const char* name)
{
    for (pugi::xml_attribute attribute = node.first_attribute(); attribute != nullptr; attribute = attribute.next_attribute()) {
        const char* full = attribute.name();
        const char* colon = std::strchr(full, ':');
        const char* local = colon != nullptr ? colon + 1 : full;
        if (std::strcmp(local, name) == 0) {
            return QString::fromUtf8(attribute.value()).trimmed();
        }
    }
    return {};
}

QString childText(const pugi::xml_node& root)
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
            if (type != pugi::node_element && type != pugi::node_document) {
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
    return text.trimmed();
}

Result<void> loadXml(pugi::xml_document& document, const QByteArray& bytes, const QString& invalidMessage)
{
    if (markupDeeperThan(bytes.constData(), static_cast<quint64>(bytes.size()), kMaxMarkupDepth)) {
        return std::unexpected(makeError(QStringLiteral("文件结构异常")));
    }
    if (!document.load_buffer(bytes.constData(), static_cast<std::size_t>(bytes.size()))) {
        return std::unexpected(makeError(invalidMessage));
    }
    return {};
}

struct ManifestItem {
    QString href;
    QString mediaType;
    QString properties;
};

struct SpineItem {
    QString href;
    bool linear = true;
};

bool isHtmlType(const QString& mediaType, const QString& href)
{
    const QString type = mediaType.toLower();
    if (type.contains(QLatin1String("html")) || type.contains(QLatin1String("xml"))) {
        return href.endsWith(QLatin1String(".xhtml"), Qt::CaseInsensitive) ||
               href.endsWith(QLatin1String(".html"), Qt::CaseInsensitive) ||
               href.endsWith(QLatin1String(".htm"), Qt::CaseInsensitive) || type.contains(QLatin1String("html"));
    }
    return href.endsWith(QLatin1String(".xhtml"), Qt::CaseInsensitive) ||
           href.endsWith(QLatin1String(".html"), Qt::CaseInsensitive) ||
           href.endsWith(QLatin1String(".htm"), Qt::CaseInsensitive);
}

Result<QString> rootfilePath(ZipArchive& zip)
{
    auto container = zip.read(QStringLiteral("META-INF/container.xml"));
    if (!container) {
        return std::unexpected(container.error());
    }
    pugi::xml_document document;
    auto loaded = loadXml(document, *container, QStringLiteral("不是有效的 EPUB"));
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    const pugi::xml_node rootfiles = childByLocal(document.document_element(), "rootfiles");
    const pugi::xml_node rootfile = childByLocal(rootfiles, "rootfile");
    const QString path = attrText(rootfile, "full-path");
    if (path.isEmpty()) {
        return std::unexpected(makeError(QStringLiteral("不是有效的 EPUB")));
    }
    const QString resolved = resolveZipPath({}, path);
    if (resolved.isEmpty()) {
        return std::unexpected(makeError(QStringLiteral("EPUB 路径不合法")));
    }
    return resolved;
}

bool fontAlgorithm(const QString& algorithm)
{
    return algorithm.contains(QLatin1String("embedding")) || algorithm.contains(QLatin1String("enc#RC"));
}

// 每一个 EncryptedData 都要有字体混淆的 method。缺 method，或 method 不是字体混淆，都是 DRM。
bool encryptionIsDrm(const pugi::xml_node& root)
{
    struct Frame {
        pugi::xml_node node;
        pugi::xml_node child;
        bool entered = false;
        bool isData = false;
        bool sawMethod = false;
        bool badMethod = false;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{root, {}, false, false, false, false});
    bool drm = false;
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (!frame.entered) {
            frame.entered = true;
            frame.isData = localName(frame.node) == QLatin1String("EncryptedData");
            frame.child = frame.node.first_child();
        }
        if (stack.empty()) {
            break;
        }
        Frame& current = stack.back();
        if (!current.child) {
            if (current.isData && (!current.sawMethod || current.badMethod)) {
                drm = true;
            }
            stack.pop_back();
            continue;
        }
        const pugi::xml_node child = current.child;
        current.child = current.child.next_sibling();
        if (localName(child) == QLatin1String("EncryptionMethod")) {
            Frame* owner = current.isData ? &current : nullptr;
            if (owner == nullptr) {
                for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                    if (it->isData) {
                        owner = &(*it);
                        break;
                    }
                }
            }
            if (owner != nullptr) {
                owner->sawMethod = true;
                if (!fontAlgorithm(attrText(child, "Algorithm"))) {
                    owner->badMethod = true;
                }
            }
        }
        stack.push_back(Frame{child, {}, false, false, false, false});
    }
    return drm;
}

Result<void> rejectDrm(ZipArchive& zip)
{
    if (!zip.contains(QStringLiteral("META-INF/encryption.xml"))) {
        return {};
    }
    auto xml = zip.read(QStringLiteral("META-INF/encryption.xml"));
    if (!xml) {
        return std::unexpected(xml.error());
    }
    pugi::xml_document document;
    auto loaded = loadXml(document, *xml, QStringLiteral("这本书有 DRM，不能打开"));
    if (!loaded) {
        return loaded;
    }
    // 字体混淆可以留着，正文仍然是明文。其他加密直接拒绝，不尝试解开。
    if (encryptionIsDrm(document)) {
        return std::unexpected(makeError(QStringLiteral("这本书有 DRM，不能打开")));
    }
    return {};
}

struct TocLink {
    QString path;
    QString fragment;
    QString title;
    quint8 level = 1;
};

QString hrefFragment(const QString& href)
{
    const qsizetype hash = href.indexOf(QLatin1Char('#'));
    if (hash < 0 || hash + 1 >= href.size()) {
        return {};
    }
    return QUrl::fromPercentEncoding(href.mid(hash + 1).toUtf8());
}

quint8 tocLevel(int depth)
{
    if (depth < 1) {
        return 1;
    }
    if (depth > 255) {
        return 255;
    }
    return static_cast<quint8>(depth);
}

void rememberLink(std::vector<TocLink>& links, const QString& baseDir, const QString& href, const QString& title, int depth)
{
    const QString path = resolveZipPath(baseDir, href);
    const QString label = title.trimmed();
    if (path.isEmpty() || label.isEmpty()) {
        return;
    }
    TocLink link;
    link.path = path;
    link.fragment = hrefFragment(href);
    link.title = label;
    link.level = tocLevel(depth);
    links.push_back(std::move(link));
}

void collectNavTitles(const pugi::xml_node& root, const QString& baseDir, std::vector<TocLink>& links)
{
    struct Frame {
        pugi::xml_node node;
        pugi::xml_node child;
        int depth = 0;
        bool entered = false;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{root, {}, 0, false});
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (!frame.entered) {
            frame.entered = true;
            if (localName(frame.node) == QLatin1String("ol")) {
                ++frame.depth;
            }
            if (localName(frame.node) == QLatin1String("a")) {
                rememberLink(links, baseDir, attrText(frame.node, "href"), childText(frame.node), frame.depth);
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
        const int depth = current.depth;
        current.child = current.child.next_sibling();
        stack.push_back(Frame{child, {}, depth, false});
    }
}

void collectNcxTitles(const pugi::xml_node& root, const QString& baseDir, std::vector<TocLink>& links)
{
    struct Frame {
        pugi::xml_node node;
        pugi::xml_node child;
        int depth = 0;
        bool entered = false;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{root, {}, 0, false});
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (!frame.entered) {
            frame.entered = true;
            if (localName(frame.node) == QLatin1String("navPoint")) {
                ++frame.depth;
                const pugi::xml_node label = childByLocal(frame.node, "navLabel");
                const pugi::xml_node content = childByLocal(frame.node, "content");
                rememberLink(links,
                             baseDir,
                             attrText(content, "src"),
                             childText(childByLocal(label, "text")),
                             frame.depth);
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
        const int depth = current.depth;
        current.child = current.child.next_sibling();
        stack.push_back(Frame{child, {}, depth, false});
    }
}

}  // namespace

Result<ParsedBook> parseEpub(const QString& path,
                             const QString& cacheDir,
                             const QString& token,
                             const std::atomic<bool>& cancel,
                             const PreviewHandler& preview)
{
    auto zip = ZipArchive::open(path);
    if (!zip) {
        return std::unexpected(zip.error());
    }
    if (cancel.load()) {
        return std::unexpected(cancelledError());
    }
    auto drm = rejectDrm(*zip);
    if (!drm) {
        return std::unexpected(drm.error());
    }
    auto opfPath = rootfilePath(*zip);
    if (!opfPath) {
        return std::unexpected(opfPath.error());
    }
    auto opfBytes = zip->read(*opfPath);
    if (!opfBytes) {
        return std::unexpected(opfBytes.error());
    }
    pugi::xml_document opf;
    auto opfLoaded = loadXml(opf, *opfBytes, QStringLiteral("不是有效的 EPUB"));
    if (!opfLoaded) {
        return std::unexpected(opfLoaded.error());
    }
    const QString opfDir = QFileInfo(*opfPath).path() == QLatin1String(".") ? QString() : QFileInfo(*opfPath).path();
    const pugi::xml_node package = opf.document_element();
    const pugi::xml_node metadata = childByLocal(package, "metadata");
    const pugi::xml_node manifest = childByLocal(package, "manifest");
    const pugi::xml_node spine = childByLocal(package, "spine");
    if (!manifest || !spine) {
        return std::unexpected(makeError(QStringLiteral("不是有效的 EPUB")));
    }

    QString title = childText(childByLocal(metadata, "title"));
    if (title.isEmpty()) {
        title = QFileInfo(path).completeBaseName();
    }
    QString author;
    for (pugi::xml_node child = metadata.first_child(); child != nullptr; child = child.next_sibling()) {
        if (localName(child) == QLatin1String("creator")) {
            author = childText(child);
            if (!author.isEmpty()) {
                break;
            }
        }
    }

    QHash<QString, ManifestItem> items;
    QString ncxHref;
    QString navHref;
    for (pugi::xml_node item = manifest.first_child(); item != nullptr; item = item.next_sibling()) {
        if (localName(item) != QLatin1String("item")) {
            continue;
        }
        ManifestItem entry;
        const QString rawHref = attrText(item, "href");
        // 远程地址和越出压缩包根目录的路径跳过，不让这一项把整本书弄失败。
        if (rawHref.isEmpty() || rawHref.startsWith(QLatin1String("//")) || rawHref.contains(QLatin1String("://"))) {
            continue;
        }
        const QString href = resolveZipPath(opfDir, rawHref);
        if (href.isEmpty()) {
            continue;
        }
        entry.href = href;
        entry.mediaType = attrText(item, "media-type");
        entry.properties = attrText(item, "properties");
        const QString id = attrText(item, "id");
        if (!id.isEmpty()) {
            items.insert(id, entry);
        }
        if (entry.mediaType.compare(QLatin1String("application/x-dtbncx+xml"), Qt::CaseInsensitive) == 0) {
            ncxHref = entry.href;
        }
        if (entry.properties.split(QLatin1Char(' '), Qt::SkipEmptyParts).contains(QLatin1String("nav"))) {
            navHref = entry.href;
        }
    }
    const QString tocId = attrText(spine, "toc");
    if (ncxHref.isEmpty() && items.contains(tocId)) {
        ncxHref = items.value(tocId).href;
    }

    std::vector<TocLink> toc;
    if (!navHref.isEmpty()) {
        auto navBytes = zip->read(navHref);
        if (navBytes) {
            pugi::xml_document nav;
            if (loadXml(nav, *navBytes, QStringLiteral("不是有效的 EPUB"))) {
                const QString navDir = QFileInfo(navHref).path() == QLatin1String(".") ? QString() : QFileInfo(navHref).path();
                collectNavTitles(nav.document_element(), navDir, toc);
            }
        }
    }
    if (toc.empty() && !ncxHref.isEmpty()) {
        auto ncxBytes = zip->read(ncxHref);
        if (ncxBytes) {
            pugi::xml_document ncx;
            if (loadXml(ncx, *ncxBytes, QStringLiteral("不是有效的 EPUB"))) {
                const QString ncxDir = QFileInfo(ncxHref).path() == QLatin1String(".") ? QString() : QFileInfo(ncxHref).path();
                collectNcxTitles(ncx.document_element(), ncxDir, toc);
            }
        }
    }

    std::vector<SpineItem> spineItems;
    for (pugi::xml_node itemref = spine.first_child(); itemref != nullptr; itemref = itemref.next_sibling()) {
        if (localName(itemref) != QLatin1String("itemref")) {
            continue;
        }
        const QString idref = attrText(itemref, "idref");
        const auto found = items.constFind(idref);
        if (found == items.cend()) {
            continue;
        }
        if (!isHtmlType(found->mediaType, found->href)) {
            continue;
        }
        SpineItem spineItem;
        spineItem.href = found->href;
        spineItem.linear = attrText(itemref, "linear").compare(QLatin1String("no"), Qt::CaseInsensitive) != 0;
        spineItems.push_back(std::move(spineItem));
        if (spineItems.size() > 10000) {
            return std::unexpected(makeError(QStringLiteral("章节太多")));
        }
    }
    bool anyLinear = false;
    for (const SpineItem& item : spineItems) {
        if (item.linear) {
            anyLinear = true;
            break;
        }
    }

    std::vector<TextChapter> chapters;
    bool previewed = false;
    for (const SpineItem& item : spineItems) {
        if (cancel.load()) {
            return std::unexpected(cancelledError());
        }
        if (anyLinear && !item.linear) {
            continue;
        }
        auto bytes = zip->read(item.href);
        if (!bytes) {
            if (bytes.error().message == QStringLiteral("压缩包里缺少文件")) {
                qWarning().noquote() << QStringLiteral("EPUB 跳过缺失的文件：") << item.href;
                continue;
            }
            return std::unexpected(bytes.error());
        }
        const QByteArray& raw = *bytes;
        if (markupDeeperThan(raw.constData(), static_cast<quint64>(raw.size()), kMaxMarkupDepth)) {
            return std::unexpected(makeError(QStringLiteral("文件结构异常")));
        }

        QStringList anchors;
        const TocLink* whole = nullptr;
        bool split = false;
        for (const TocLink& link : toc) {
            if (link.path != item.href) {
                continue;
            }
            if (link.fragment.isEmpty()) {
                if (whole == nullptr) {
                    whole = &link;
                }
                continue;
            }
            split = true;
            if (!anchors.contains(link.fragment)) {
                anchors.push_back(link.fragment);
            }
        }

        const QString filename = QFileInfo(item.href).completeBaseName();
        auto appendPlain = [&](QString chapterTitle, quint8 level, const QString& plain) -> Result<void> {
            if (plain.trimmed().isEmpty()) {
                return {};
            }
            if (chapters.size() >= 10000) {
                return std::unexpected(makeError(QStringLiteral("章节太多")));
            }
            if (chapterTitle.isEmpty()) {
                chapterTitle = filename.isEmpty() ? QStringLiteral("正文") : filename;
            }
            TextChapter chapter;
            chapter.title = std::move(chapterTitle);
            chapter.level = level == 0 ? 2 : level;
            chapter.utf8 = plain.toUtf8();
            if (!previewed && preview) {
                ChapterIndex meta;
                meta.title = chapter.title;
                meta.byteEnd = static_cast<quint64>(chapter.utf8.size());
                preview(makeChapterContent(meta, chapter.utf8.constData(), static_cast<quint64>(chapter.utf8.size())),
                        0,
                        static_cast<quint64>(chapter.utf8.size()));
                previewed = true;
            }
            chapters.push_back(std::move(chapter));
            return {};
        };

        auto appendLenient = [&](const QString& headingFallback) -> Result<void> {
            const MarkupText lenient = htmlToPlainLenient(raw.constData(), static_cast<quint64>(raw.size()), &cancel);
            if (cancel.load()) {
                return std::unexpected(cancelledError());
            }
            QString title = whole != nullptr ? whole->title : headingFallback;
            if (title.isEmpty()) {
                title = lenient.firstHeading;
            }
            const quint8 level = whole != nullptr ? whole->level : 2;
            return appendPlain(title, level, lenient.text);
        };

        if (split) {
            auto slices = xhtmlToAnchoredSlices(raw.constData(), static_cast<quint64>(raw.size()), anchors);
            if (!slices) {
                if (slices.error().message == QStringLiteral("文件结构异常")) {
                    return std::unexpected(slices.error());
                }
                auto added = appendLenient({});
                if (!added) {
                    return std::unexpected(added.error());
                }
                continue;
            }
            for (const MarkupSlice& slice : *slices) {
                const TocLink* matched = nullptr;
                for (const TocLink& link : toc) {
                    if (link.path == item.href && link.fragment == slice.anchor) {
                        matched = &link;
                        break;
                    }
                }
                const QString sliceTitle = matched != nullptr ? matched->title : QString();
                const quint8 level = matched != nullptr ? matched->level : 2;
                auto added = appendPlain(sliceTitle, level, slice.text);
                if (!added) {
                    return std::unexpected(added.error());
                }
            }
            continue;
        }

        QString plain;
        QString heading;
        auto xhtml = xhtmlToPlain(raw.constData(), static_cast<quint64>(raw.size()));
        if (xhtml) {
            plain = std::move(xhtml->text);
            heading = std::move(xhtml->firstHeading);
        } else if (xhtml.error().message == QStringLiteral("文件结构异常")) {
            return std::unexpected(xhtml.error());
        } else {
            auto added = appendLenient(heading);
            if (!added) {
                return std::unexpected(added.error());
            }
            continue;
        }
        const QString chapterTitle = whole != nullptr ? whole->title : heading;
        const quint8 level = whole != nullptr ? whole->level : 2;
        auto added = appendPlain(chapterTitle, level, plain);
        if (!added) {
            return std::unexpected(added.error());
        }
    }

    Book book;
    book.filePath = path;
    book.fingerprint = token;
    book.title = title;
    book.author = author;
    book.format = BookFormat::Epub;
    book.lastReadAt = QDateTime::currentMSecsSinceEpoch();
    return writeBodyCache(std::move(book), chapters, cacheDir, token, cancel);
}

}  // namespace moyu
