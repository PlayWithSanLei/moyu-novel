#include "parser/Markup.h"

#include <QStringList>

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

#include <pugixml.hpp>

namespace moyu {
namespace {

QString expandEntities(const QString& raw);

// 只折叠 ASCII 空白。nbsp（U+00A0）是可见字符，不能收成普通空格。
bool isCollapsibleSpace(QChar character)
{
    return character == QLatin1Char(' ') || character == QLatin1Char('\t') || character == QLatin1Char('\n') ||
           character == QLatin1Char('\f') || character == QLatin1Char('\v');
}

class TextBuilder {
public:
    void newline()
    {
        if (!m_text.isEmpty() && !m_text.endsWith(QLatin1Char('\n'))) {
            m_text += QLatin1Char('\n');
        }
    }

    void breakLine() { m_text += QLatin1Char('\n'); }

    void markHeading()
    {
        newline();
        m_text += QChar(static_cast<char16_t>(static_cast<unsigned char>(kHeadingMark)));
    }

    void addText(const QString& raw, bool pre)
    {
        const QString expanded = expandEntities(raw);
        if (pre) {
            m_text += expanded;
            return;
        }
        // 没有可折叠空白时整段接上。16MB 的 & 不能逐字 append。
        // nbsp 要留下，不能收成普通空格。
        bool plain = true;
        for (const QChar character : expanded) {
            if (character == QLatin1Char('\r') || isCollapsibleSpace(character)) {
                plain = false;
                break;
            }
        }
        if (plain) {
            m_text += expanded;
            return;
        }
        for (QChar character : expanded) {
            if (character == QLatin1Char('\r')) {
                continue;
            }
            if (isCollapsibleSpace(character)) {
                if (m_text.isEmpty() || m_text.endsWith(QLatin1Char('\n')) || m_text.endsWith(QLatin1Char(' '))) {
                    continue;
                }
                m_text += QLatin1Char(' ');
                continue;
            }
            m_text += character;
        }
    }

    QString take() const { return m_text; }

    void appendUnchanged(const QString& raw) { m_text += raw; }

    void reset() { m_text.clear(); }

private:
    QString m_text;
};

struct NamedEntity {
    const char* name;
    char32_t code;
};

// 两条 HTML 路径共用。只收阅读里常见的命名实体，数值实体另外解。
constexpr NamedEntity kNamedEntities[] = {
    {"amp", U'&'},
    {"apos", U'\''},
    {"copy", U'\u00A9'},
    {"gt", U'>'},
    {"hellip", U'\u2026'},
    {"ldquo", U'\u201C'},
    {"lsquo", U'\u2018'},
    {"lt", U'<'},
    {"mdash", U'\u2014'},
    {"middot", U'\u00B7'},
    {"nbsp", U'\u00A0'},
    {"ndash", U'\u2013'},
    {"quot", U'"'},
    {"rdquo", U'\u201D'},
    {"rsquo", U'\u2019'},
};

QString decodeEntity(QStringView name)
{
    for (const NamedEntity& entity : kNamedEntities) {
        if (name == QLatin1String(entity.name)) {
            return QString(QChar::fromUcs4(entity.code));
        }
    }
    bool ok = false;
    uint code = 0;
    if (name.startsWith(QLatin1Char('#'))) {
        if (name.size() > 1 && (name.at(1) == QLatin1Char('x') || name.at(1) == QLatin1Char('X'))) {
            code = name.mid(2).toUInt(&ok, 16);
        } else {
            code = name.mid(1).toUInt(&ok, 10);
        }
    }
    if (!ok || code == 0 || code > 0x10FFFF) {
        return {};
    }
    return QString(QChar::fromUcs4(code));
}

QString expandEntities(const QString& raw)
{
    if (!raw.contains(QLatin1Char('&')) || !raw.contains(QLatin1Char(';'))) {
        return raw;
    }
    QString out;
    out.reserve(raw.size());
    const qsizetype length = raw.size();
    for (qsizetype i = 0; i < length;) {
        if (raw.at(i) != QLatin1Char('&')) {
            out += raw.at(i);
            ++i;
            continue;
        }
        const qsizetype end = std::min(length, i + 12);
        qsizetype semi = -1;
        for (qsizetype j = i + 1; j < end; ++j) {
            if (raw.at(j) == QLatin1Char(';')) {
                semi = j;
                break;
            }
        }
        if (semi < 0) {
            out += QLatin1Char('&');
            ++i;
            continue;
        }
        const QString decoded = decodeEntity(QStringView(raw).mid(i + 1, semi - i - 1));
        if (decoded.isEmpty()) {
            out += QLatin1Char('&');
            ++i;
            continue;
        }
        out += decoded;
        i = semi + 1;
    }
    return out;
}

bool localIs(const pugi::xml_node& node, const char* name)
{
    const char* full = node.name();
    const char* colon = std::strchr(full, ':');
    const char* local = colon != nullptr ? colon + 1 : full;
    return std::strcmp(local, name) == 0;
}

bool localOneOf(const pugi::xml_node& node, std::initializer_list<const char*> names)
{
    for (const char* name : names) {
        if (localIs(node, name)) {
            return true;
        }
    }
    return false;
}

bool isSkipped(const pugi::xml_node& node)
{
    return localOneOf(node, {"script", "style", "head"});
}

bool isHeading(const pugi::xml_node& node)
{
    return localOneOf(node, {"h1", "h2", "h3", "h4", "h5", "h6"});
}

bool isBlock(const pugi::xml_node& node)
{
    return isHeading(node) ||
           localOneOf(node,
                      {"address", "article", "aside", "blockquote", "div", "dd", "dt", "figcaption", "figure",
                       "footer", "header", "hr", "li", "nav", "ol", "p", "pre", "section", "table", "tr", "ul",
                       "body", "title"});
}

QString attributeLocal(const pugi::xml_node& node, const char* name)
{
    for (pugi::xml_attribute attribute = node.first_attribute(); attribute != nullptr; attribute = attribute.next_attribute()) {
        const char* full = attribute.name();
        const char* colon = std::strchr(full, ':');
        const char* local = colon != nullptr ? colon + 1 : full;
        if (std::strcmp(local, name) == 0) {
            return QString::fromUtf8(attribute.value());
        }
    }
    return {};
}

QString elementAnchor(const pugi::xml_node& node)
{
    const QString id = attributeLocal(node, "id");
    if (!id.isEmpty()) {
        return id;
    }
    return attributeLocal(node, "name");
}

// 显式栈。加载线程栈只有 512KB，按元素深度递归会在几万层时溢出。
void walk(const pugi::xml_node& root, TextBuilder& builder, QStringList anchors, std::vector<MarkupSlice>* slices)
{
    struct Frame {
        pugi::xml_node node;
        pugi::xml_node child;
        bool pre = false;
        bool block = false;
        bool entered = false;
    };
    QString currentAnchor;
    auto flush = [&] {
        if (slices == nullptr) {
            return;
        }
        MarkupSlice slice;
        slice.anchor = currentAnchor;
        slice.text = builder.take();
        builder.reset();
        slices->push_back(std::move(slice));
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{root, {}, false, false, false});
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (!frame.entered) {
            frame.entered = true;
            const pugi::xml_node_type type = frame.node.type();
            if (type == pugi::node_pcdata || type == pugi::node_cdata) {
                builder.addText(QString::fromUtf8(frame.node.value()), frame.pre);
                stack.pop_back();
                continue;
            }
            if (type != pugi::node_element && type != pugi::node_document) {
                stack.pop_back();
                continue;
            }
            if (type == pugi::node_element && isSkipped(frame.node)) {
                stack.pop_back();
                continue;
            }
            if (type == pugi::node_element && localIs(frame.node, "br")) {
                builder.breakLine();
                stack.pop_back();
                continue;
            }
            if (type == pugi::node_element && slices != nullptr) {
                const QString anchor = elementAnchor(frame.node);
                if (!anchor.isEmpty()) {
                    const qsizetype index = anchors.indexOf(anchor);
                    if (index >= 0) {
                        flush();
                        currentAnchor = anchor;
                        anchors.removeAt(index);
                    }
                }
            }
            frame.block = type == pugi::node_element && isBlock(frame.node);
            const bool heading = type == pugi::node_element && isHeading(frame.node);
            const bool nowPre = frame.pre || (type == pugi::node_element && localIs(frame.node, "pre"));
            if (frame.block) {
                builder.newline();
            }
            if (heading) {
                builder.markHeading();
            }
            frame.pre = nowPre;
            frame.child = frame.node.first_child();
        }
        if (stack.empty()) {
            break;
        }
        Frame& current = stack.back();
        if (!current.child) {
            if (current.block) {
                builder.newline();
            }
            stack.pop_back();
            continue;
        }
        const pugi::xml_node child = current.child;
        const bool childPre = current.pre;
        current.child = current.child.next_sibling();
        stack.push_back(Frame{child, {}, childPre, false, false});
    }
    if (slices != nullptr) {
        flush();
    }
}

bool isBlockTag(QStringView tag)
{
    return tag == QLatin1String("p") || tag == QLatin1String("div") || tag == QLatin1String("li") ||
           tag == QLatin1String("tr") || tag == QLatin1String("blockquote") || tag == QLatin1String("section") ||
           tag == QLatin1String("article") || tag == QLatin1String("h1") || tag == QLatin1String("h2") ||
           tag == QLatin1String("h3") || tag == QLatin1String("h4") || tag == QLatin1String("h5") ||
           tag == QLatin1String("h6") || tag == QLatin1String("pre") || tag == QLatin1String("header");
}

bool isHeadingTag(QStringView tag)
{
    return tag.size() == 2 && tag.at(0) == QLatin1Char('h') && tag.at(1) >= QLatin1Char('1') && tag.at(1) <= QLatin1Char('6');
}

}  // namespace

bool markupDeeperThan(const char* data, quint64 size, int limit)
{
    if (data == nullptr || size == 0 || limit < 1) {
        return false;
    }
    int depth = 0;
    for (quint64 i = 0; i < size; ++i) {
        if (data[i] != '<') {
            continue;
        }
        if (i + 1 >= size) {
            break;
        }
        if (i + 3 < size && data[i + 1] == '!' && data[i + 2] == '-' && data[i + 3] == '-') {
            i += 4;
            while (i + 2 < size && !(data[i] == '-' && data[i + 1] == '-' && data[i + 2] == '>')) {
                ++i;
            }
            continue;
        }
        if (i + 8 < size && std::memcmp(data + i, "<![CDATA[", 9) == 0) {
            i += 9;
            while (i + 2 < size && !(data[i] == ']' && data[i + 1] == ']' && data[i + 2] == '>')) {
                ++i;
            }
            continue;
        }
        if (data[i + 1] == '!' || data[i + 1] == '?') {
            while (i < size && data[i] != '>') {
                ++i;
            }
            continue;
        }
        const bool closing = data[i + 1] == '/';
        bool selfClosing = false;
        char quote = 0;
        quint64 j = i + 1;
        for (; j < size; ++j) {
            const char character = data[j];
            if (quote != 0) {
                if (character == quote) {
                    quote = 0;
                }
                continue;
            }
            if (character == '"' || character == '\'') {
                quote = character;
                continue;
            }
            if (character == '>') {
                quint64 k = j;
                while (k > i && (data[k - 1] == ' ' || data[k - 1] == '\t' || data[k - 1] == '\n' || data[k - 1] == '\r')) {
                    --k;
                }
                selfClosing = k > i && data[k - 1] == '/';
                break;
            }
        }
        if (j >= size) {
            break;
        }
        if (closing) {
            if (depth > 0) {
                --depth;
            }
        } else if (!selfClosing) {
            ++depth;
            if (depth > limit) {
                return true;
            }
        }
        i = j;
    }
    return false;
}

Result<std::vector<MarkupSlice>> loadSlices(const char* data, quint64 size, const QStringList& anchors)
{
    if (data == nullptr || size == 0 || size > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        return std::unexpected(makeError(QStringLiteral("章节是空的")));
    }
    if (markupDeeperThan(data, size, kMaxMarkupDepth)) {
        return std::unexpected(makeError(QStringLiteral("文件结构异常")));
    }
    pugi::xml_document document;
    const pugi::xml_parse_result parsed = document.load_buffer(data, static_cast<std::size_t>(size), pugi::parse_default);
    if (!parsed) {
        return std::unexpected(makeError(QStringLiteral("章节 XML 无法解析")));
    }
    TextBuilder builder;
    std::vector<MarkupSlice> slices;
    walk(document, builder, anchors, anchors.isEmpty() ? nullptr : &slices);
    if (anchors.isEmpty()) {
        MarkupSlice slice;
        slice.text = builder.take();
        slices.push_back(std::move(slice));
    }
    return slices;
}

Result<MarkupText> xhtmlToPlain(const char* data, quint64 size)
{
    auto slices = loadSlices(data, size, {});
    if (!slices) {
        return std::unexpected(slices.error());
    }
    MarkupText text;
    if (!slices->empty()) {
        text.text = slices->front().text;
    }
    text.firstHeading = firstMarkedHeading(text.text);
    return text;
}

Result<std::vector<MarkupSlice>> xhtmlToAnchoredSlices(const char* data, quint64 size, const QStringList& anchors)
{
    return loadSlices(data, size, anchors);
}

MarkupText htmlToPlainLenient(const char* data, quint64 size, const std::atomic<bool>* cancel)
{
    MarkupText text;
    if (data == nullptr || size == 0 || size > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        return text;
    }
    const QString html = QString::fromUtf8(data, static_cast<qsizetype>(size));
    TextBuilder builder;
    bool skipping = false;
    QString skipTag;
    const qsizetype length = html.size();
    qsizetype steps = 0;
    for (qsizetype i = 0; i < length;) {
        if ((++steps & 4095) == 0 && cancel != nullptr && cancel->load()) {
            break;
        }
        if (html.at(i) != QLatin1Char('<')) {
            if (!skipping) {
                const qsizetype start = i;
                while (i < length && html.at(i) != QLatin1Char('<') && html.at(i) != QLatin1Char('&')) {
                    ++i;
                }
                if (i == start && html.at(i) == QLatin1Char('&')) {
                    // 连续的 & 不可能是实体名的一部分，一次接上，避免每个 & 都扫到文件尾。
                    qsizetype run = i + 1;
                    while (run < length && html.at(run) == QLatin1Char('&')) {
                        ++run;
                        if (((run - i) & 4095) == 0 && cancel != nullptr && cancel->load()) {
                            break;
                        }
                    }
                    if (cancel != nullptr && cancel->load()) {
                        break;
                    }
                    if (run > i + 1) {
                        builder.appendUnchanged(html.mid(i, run - i - 1));
                        i = run - 1;
                    }
                    qsizetype semi = -1;
                    const qsizetype end = std::min(length, i + 12);
                    for (qsizetype j = i + 1; j < end; ++j) {
                        if (html.at(j) == QLatin1Char(';')) {
                            semi = j;
                            break;
                        }
                    }
                    if (semi > i) {
                        builder.addText(decodeEntity(QStringView(html).mid(i + 1, semi - i - 1)), false);
                        i = semi + 1;
                        continue;
                    }
                    builder.addText(QStringLiteral("&"), false);
                    ++i;
                    continue;
                }
                builder.addText(html.mid(start, i - start), false);
                continue;
            }
            ++i;
            continue;
        }
        const qsizetype close = html.indexOf(QLatin1Char('>'), i + 1);
        if (close < 0) {
            break;
        }
        QStringView tag = QStringView(html).mid(i + 1, close - i - 1).trimmed();
        i = close + 1;
        if (tag.startsWith(QLatin1Char('/'))) {
            tag = tag.mid(1).trimmed();
            const QString name = tag.toString().toLower();
            if (skipping && name == skipTag) {
                skipping = false;
                skipTag.clear();
            } else if (!skipping && isBlockTag(name)) {
                builder.newline();
            }
            continue;
        }
        bool selfClosing = false;
        qsizetype tagEnd = tag.size();
        while (tagEnd > 0 && tag.at(tagEnd - 1).isSpace()) {
            --tagEnd;
        }
        if (tagEnd > 0 && tag.at(tagEnd - 1) == QLatin1Char('/')) {
            selfClosing = true;
            tag = tag.left(tagEnd - 1).trimmed();
        }
        const QString raw = tag.toString().toLower();
        const QString name = raw.section(QLatin1Char(' '), 0, 0);
        if (!selfClosing && (name == QLatin1String("script") || name == QLatin1String("style"))) {
            skipping = true;
            skipTag = name;
            continue;
        }
        if (skipping) {
            continue;
        }
        if (name == QLatin1String("br") || name == QLatin1String("hr")) {
            builder.breakLine();
            continue;
        }
        if (isHeadingTag(name)) {
            builder.markHeading();
            continue;
        }
        if (isBlockTag(name)) {
            builder.newline();
        }
    }
    text.text = builder.take();
    text.firstHeading = firstMarkedHeading(text.text);
    return text;
}

QString firstMarkedHeading(const QString& text)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    const QChar mark(static_cast<char16_t>(static_cast<unsigned char>(kHeadingMark)));
    for (const QString& line : lines) {
        if (line.startsWith(mark)) {
            const QString heading = line.mid(1).trimmed();
            if (!heading.isEmpty()) {
                return heading;
            }
        }
    }
    return {};
}

std::vector<TextChapter> chaptersFromMarkedText(const QString& text, const QString& bookTitle)
{
    std::vector<TextChapter> chapters;
    TextChapter current;
    current.title = bookTitle.isEmpty() ? QStringLiteral("正文") : bookTitle;
    const QChar mark(static_cast<char16_t>(static_cast<unsigned char>(kHeadingMark)));
    const QStringList lines = text.split(QLatin1Char('\n'));
    auto flush = [&] {
        const QString trimmed = current.utf8.isEmpty() ? QString() : QString::fromUtf8(current.utf8).trimmed();
        if (!trimmed.isEmpty()) {
            chapters.push_back(std::move(current));
        }
        current = TextChapter{};
    };
    for (const QString& line : lines) {
        if (line.startsWith(mark)) {
            const QString heading = line.mid(1).trimmed();
            const bool currentHasBody = !QString::fromUtf8(current.utf8).trimmed().isEmpty();
            if (currentHasBody || !chapters.empty()) {
                flush();
            }
            if (!heading.isEmpty()) {
                current.title = heading;
                current.utf8 += heading.toUtf8();
                current.utf8 += '\n';
            }
            continue;
        }
        current.utf8 += line.toUtf8();
        current.utf8 += '\n';
    }
    flush();
    if (chapters.empty() && !text.trimmed().isEmpty()) {
        TextChapter only;
        only.title = bookTitle.isEmpty() ? QStringLiteral("正文") : bookTitle;
        only.utf8 = text.toUtf8();
        chapters.push_back(std::move(only));
    }
    return chapters;
}

}  // namespace moyu
