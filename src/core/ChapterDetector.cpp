#include "core/ChapterDetector.h"

#include "core/Utf8.h"

#include <QRegularExpression>
#include <QStringView>

#include <cstring>
#include <limits>
#include <optional>

namespace moyu {
namespace {

enum class RuleKind { Chinese, English, Aux };

struct Hit {
    quint64 lineStart = 0;
    quint64 trimBegin = 0;
    quint64 trimEnd = 0;
    quint8 level = 2;
};

struct RuleMatch {
    quint8 level = 2;
};

bool tooDense(int matches, int nonEmpty)
{
    // 两行里只有一个真标题时密度是 100%，不能因此丢掉。
    // 至少 8 处，并且超过非空行的 30%，才当成「整篇都像标题」。
    return matches >= 8 && matches * 10 > nonEmpty * 3;
}

bool isOnlyWhitespace(const char* begin, const char* end)
{
    const char* cursor = begin;
    while (cursor < end) {
        const auto byte = static_cast<unsigned char>(*cursor);
        if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\f' || byte == '\v') {
            ++cursor;
            continue;
        }
        if (byte == 0xE3 && cursor + 3 <= end && static_cast<unsigned char>(cursor[1]) == 0x80 &&
            static_cast<unsigned char>(cursor[2]) == 0x80) {
            cursor += 3;
            continue;
        }
        return false;
    }
    return true;
}

void trimLine(const char* line, const char* newline, const char** begin, const char** end)
{
    const char* left = line;
    const char* right = newline;
    while (left < right && (*left == ' ' || *left == '\t')) {
        ++left;
    }
    while (right > left && (right[-1] == ' ' || right[-1] == '\t' || right[-1] == '\r')) {
        --right;
    }
    while (left + 3 <= right && static_cast<unsigned char>(left[0]) == 0xE3 &&
           static_cast<unsigned char>(left[1]) == 0x80 && static_cast<unsigned char>(left[2]) == 0x80) {
        left += 3;
    }
    while (right - 3 >= left && static_cast<unsigned char>(right[-3]) == 0xE3 &&
           static_cast<unsigned char>(right[-2]) == 0x80 && static_cast<unsigned char>(right[-1]) == 0x80) {
        right -= 3;
    }
    *begin = left;
    *end = right;
}

bool startsWith(const char* begin, const char* end, const char* bytes, int length)
{
    return end - begin >= length && std::memcmp(begin, bytes, static_cast<std::size_t>(length)) == 0;
}

bool chinesePrefix(const char* begin, const char* end)
{
    return startsWith(begin, end, "\xE7\xAC\xAC", 3);
}

bool englishPrefix(const char* begin, const char* end)
{
    return end > begin && (*begin == 'C' || *begin == 'c') && end - begin >= 7;
}

bool auxPrefix(const char* begin, const char* end)
{
    static const char* words[] = {
        "\xE5\xBA\x8F\xE7\xAB\xA0",  // 序章
        "\xE6\xA5\x94\xE5\xAD\x90",  // 楔子
        "\xE5\xBC\x95\xE5\xAD\x90",  // 引子
        "\xE5\x89\x8D\xE8\xA8\x80",  // 前言
        "\xE5\x90\x8E\xE8\xAE\xB0",  // 后记
        "\xE5\xB0\xBE\xE5\xA3\xB0",  // 尾声
        "\xE7\x95\xAA\xE5\xA4\x96",  // 番外
    };
    for (const char* word : words) {
        if (startsWith(begin, end, word, 6)) {
            return true;
        }
    }
    return false;
}

const QRegularExpression& chinesePattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^第[零一二三四五六七八九十百千万两0-9０-９]+([章节回卷集部篇])"));
    return pattern;
}

const QRegularExpression& englishPattern()
{
    static const QRegularExpression pattern(QStringLiteral(R"(^chapter\s+(\d+|[IVXLC]+)\b)"),
                                             QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

std::optional<RuleMatch> matchChinese(const QString& line)
{
    const QRegularExpressionMatch matched = chinesePattern().match(line);
    if (!matched.hasMatch()) {
        return std::nullopt;
    }
    RuleMatch result;
    result.level = matched.capturedView(1) == QStringView(u"卷") ? 1 : 2;
    return result;
}

std::optional<RuleMatch> matchEnglish(const QString& line)
{
    if (!englishPattern().match(line).hasMatch()) {
        return std::nullopt;
    }
    return RuleMatch{2};
}

bool matchAux(const QString& line)
{
    static const QStringView words[] = {u"序章", u"楔子", u"引子", u"前言", u"后记", u"尾声", u"番外"};
    const QStringView view(line);
    for (const QStringView word : words) {
        if (view == word) {
            return true;
        }
        if (view.size() > word.size() && view.startsWith(word)) {
            const QChar separator = view.at(word.size());
            if (separator == QLatin1Char(' ') || separator == QLatin1Char(':') || separator == QChar(u'：') ||
                separator == QChar(u'、') || separator == QChar(u'，') || separator == QLatin1Char(',')) {
                return true;
            }
        }
    }
    return false;
}

std::optional<RuleKind> classify(const char* begin, const char* end, quint8* level)
{
    const bool maybe = chinesePrefix(begin, end) || englishPrefix(begin, end) || auxPrefix(begin, end);
    if (!maybe) {
        return std::nullopt;
    }
    const auto length = static_cast<quint64>(end - begin);
    // 40 个码点最多 160 字节。更长的行不用跑正则。
    if (length > 160 || countCodePoints(begin, end, 41) > 40) {
        return std::nullopt;
    }
    const QString line = QString::fromUtf8(begin, static_cast<qsizetype>(length));
    if (chinesePrefix(begin, end)) {
        if (const auto matched = matchChinese(line)) {
            *level = matched->level;
            return RuleKind::Chinese;
        }
    }
    if (englishPrefix(begin, end)) {
        if (const auto matched = matchEnglish(line)) {
            *level = matched->level;
            return RuleKind::English;
        }
    }
    if (auxPrefix(begin, end) && matchAux(line)) {
        *level = 2;
        return RuleKind::Aux;
    }
    return std::nullopt;
}

struct Line {
    const char* begin = nullptr;
    const char* contentEnd = nullptr;
    const char* next = nullptr;
};

// \n、单独的 \r、\r\n 都算一行。next 停在换行之后。
Line takeLine(const char* cursor, const char* end)
{
    const char* scan = cursor;
    while (scan < end && *scan != '\n' && *scan != '\r') {
        ++scan;
    }
    Line line;
    line.begin = cursor;
    line.contentEnd = scan;
    if (scan < end && *scan == '\r') {
        ++scan;
        if (scan < end && *scan == '\n') {
            ++scan;
        }
    } else if (scan < end && *scan == '\n') {
        ++scan;
    }
    line.next = scan;
    return line;
}

const char* nextPieceEnd(const char* begin, const char* end)
{
    const char* cursor = begin;
    int codePoints = 0;
    while (cursor < end) {
        const char* paragraph = cursor;
        int paragraphPoints = 0;
        const char* scan = cursor;
        while (scan < end && *scan != '\n' && *scan != '\r') {
            const int consumed = utf8SequenceLength(scan, end);
            if (consumed <= 0) {
                break;
            }
            scan += consumed;
            ++paragraphPoints;
            if (codePoints == 0 && paragraphPoints >= kSyntheticChapterCodePoints) {
                return scan;
            }
        }
        if (scan < end && (*scan == '\n' || *scan == '\r')) {
            if (*scan == '\r' && scan + 1 < end && scan[1] == '\n') {
                scan += 2;
            } else {
                ++scan;
            }
            ++paragraphPoints;
        }
        if (codePoints > 0 && codePoints + paragraphPoints > kSyntheticChapterCodePoints) {
            return paragraph;
        }
        codePoints += paragraphPoints;
        cursor = scan;
        if (codePoints >= kSyntheticChapterCodePoints) {
            return cursor;
        }
        if (cursor == paragraph) {
            return end;
        }
    }
    return end;
}

std::vector<ChapterIndex> splitOne(const char* body, quint64 bodySize, const ChapterIndex& chapter)
{
    if (chapter.byteEnd < chapter.byteStart || chapter.byteEnd > bodySize) {
        return {chapter};
    }
    const char* begin = body + chapter.byteStart;
    const char* end = body + chapter.byteEnd;
    const int counted = countCodePoints(begin, end, kMaxChapterCodePoints + 1);
    if (counted <= kMaxChapterCodePoints) {
        return {chapter};
    }

    std::vector<ChapterIndex> pieces;
    const char* cursor = begin;
    int pieceNo = 1;
    while (cursor < end) {
        const char* pieceEnd = nextPieceEnd(cursor, end);
        if (pieceEnd <= cursor) {
            const int consumed = utf8SequenceLength(cursor, end);
            pieceEnd = consumed > 0 ? cursor + consumed : end;
        }
        ChapterIndex piece = chapter;
        piece.byteStart = chapter.byteStart + static_cast<quint64>(cursor - begin);
        piece.byteEnd = chapter.byteStart + static_cast<quint64>(pieceEnd - begin);
        piece.synthetic = pieceNo == 1 ? chapter.synthetic : true;
        if (pieceNo > 1) {
            piece.title = chapter.title + QStringLiteral(" · ") + QString::number(pieceNo);
            // 卷是 level 1。拆开的后续块不再占卷这一级。
            if (piece.level == 1) {
                piece.level = 2;
            }
        }
        pieces.push_back(std::move(piece));
        cursor = pieceEnd;
        ++pieceNo;
    }
    return pieces;
}

bool titleFits(const char* begin, const char* end)
{
    const auto length = static_cast<quint64>(end - begin);
    // 40 个码点最多 160 字节。更长的行不用跑正则。
    return length <= 160 && countCodePoints(begin, end, 41) <= 40;
}

// 必须从去掉空白后的行首开始匹配，避免正文中间的字被当成标题。
bool matchesCustom(const QRegularExpression& pattern, const char* begin, const char* end)
{
    if (!titleFits(begin, end)) {
        return false;
    }
    const QString line = QString::fromUtf8(begin, static_cast<qsizetype>(end - begin));
    const QRegularExpressionMatch matched =
        pattern.match(line, 0, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
    return matched.hasMatch();
}

}  // namespace

bool chapterRangesValid(const std::vector<ChapterIndex>& chapters, quint64 bodySize)
{
    if (chapters.empty() || bodySize == 0) {
        return false;
    }
    if (chapters.front().byteStart != 0 || chapters.back().byteEnd != bodySize) {
        return false;
    }
    for (std::size_t i = 0; i < chapters.size(); ++i) {
        const ChapterIndex& chapter = chapters[i];
        if (chapter.index != i || chapter.byteEnd <= chapter.byteStart || chapter.byteEnd > bodySize) {
            return false;
        }
        if (i + 1 < chapters.size() && chapters[i + 1].byteStart != chapter.byteEnd) {
            return false;
        }
    }
    return true;
}

Result<std::vector<ChapterIndex>> detectChapters(const char* utf8,
                                                  quint64 size,
                                                  const std::atomic<bool>* cancel,
                                                  const QString& customPattern)
{
    if (utf8 == nullptr || size == 0) {
        return std::unexpected(makeError(QStringLiteral("文件是空的")));
    }

    QRegularExpression customRe;
    const bool customReady = !customPattern.isEmpty();
    if (customReady) {
        customRe.setPattern(customPattern);
        if (!customRe.isValid()) {
            return std::unexpected(makeError(QStringLiteral("章节正则无效")));
        }
    }

    int nonEmpty = 0;
    int counts[3] = {0, 0, 0};
    int customCount = 0;
    int lines = 0;
    const char* cursor = utf8;
    const char* end = utf8 + size;
    while (cursor < end) {
        if ((lines & 1023) == 0 && cancel != nullptr && cancel->load()) {
            return std::unexpected(cancelledError());
        }
        ++lines;
        const Line line = takeLine(cursor, end);
        const char* trimBegin = nullptr;
        const char* trimEnd = nullptr;
        trimLine(line.begin, line.contentEnd, &trimBegin, &trimEnd);
        if (line.next == cursor) {
            break;
        }
        cursor = line.next;
        if (trimBegin == trimEnd) {
            continue;
        }
        ++nonEmpty;
        quint8 level = 2;
        const auto kind = classify(trimBegin, trimEnd, &level);
        if (customReady && matchesCustom(customRe, trimBegin, trimEnd)) {
            ++customCount;
        }
        if (!kind) {
            continue;
        }
        ++counts[static_cast<int>(*kind)];
    }

    const bool customWins = customReady && customCount > 0 && !tooDense(customCount, nonEmpty);
    int winner = -1;
    int winnerCount = 0;
    for (int rule = 0; rule < 3; ++rule) {
        if (tooDense(counts[rule], nonEmpty)) {
            continue;
        }
        if (counts[rule] > winnerCount) {
            winnerCount = counts[rule];
            winner = rule;
        }
    }

    std::vector<Hit> hits;
    if (customWins || winner >= 0) {
        cursor = utf8;
        lines = 0;
        while (cursor < end) {
            if ((lines & 1023) == 0 && cancel != nullptr && cancel->load()) {
                return std::unexpected(cancelledError());
            }
            ++lines;
            const quint64 lineStart = static_cast<quint64>(cursor - utf8);
            const Line line = takeLine(cursor, end);
            const char* trimBegin = nullptr;
            const char* trimEnd = nullptr;
            trimLine(line.begin, line.contentEnd, &trimBegin, &trimEnd);
            if (line.next == cursor) {
                break;
            }
            cursor = line.next;
            if (trimBegin == trimEnd) {
                continue;
            }
            quint8 level = 2;
            if (customWins) {
                if (!matchesCustom(customRe, trimBegin, trimEnd)) {
                    continue;
                }
            } else {
                const auto kind = classify(trimBegin, trimEnd, &level);
                const bool auxWithPrimary = kind && *kind == RuleKind::Aux &&
                                            (winner == static_cast<int>(RuleKind::Chinese) ||
                                             winner == static_cast<int>(RuleKind::English));
                if (!kind || (static_cast<int>(*kind) != winner && !auxWithPrimary)) {
                    continue;
                }
            }
            Hit hit;
            hit.lineStart = lineStart;
            hit.trimBegin = static_cast<quint64>(trimBegin - utf8);
            hit.trimEnd = static_cast<quint64>(trimEnd - utf8);
            hit.level = level;
            hits.push_back(hit);
        }
    }

    std::vector<ChapterIndex> chapters;
    if (hits.empty()) {
        ChapterIndex whole;
        whole.title = QStringLiteral("正文");
        whole.level = 2;
        whole.byteStart = 0;
        whole.byteEnd = size;
        chapters.push_back(std::move(whole));
    } else {
        const bool absorb = hits.front().lineStart > 0 && isOnlyWhitespace(utf8, utf8 + hits.front().lineStart);
        if (hits.front().lineStart > 0 && !absorb) {
            ChapterIndex preamble;
            preamble.title = QStringLiteral("开头");
            preamble.level = 2;
            preamble.byteStart = 0;
            preamble.byteEnd = hits.front().lineStart;
            chapters.push_back(std::move(preamble));
        }
        for (std::size_t i = 0; i < hits.size(); ++i) {
            ChapterIndex chapter;
            const auto titleLength = static_cast<qsizetype>(hits[i].trimEnd - hits[i].trimBegin);
            chapter.title = QString::fromUtf8(utf8 + hits[i].trimBegin, titleLength);
            chapter.level = hits[i].level;
            chapter.byteStart = (i == 0 && absorb) ? 0 : hits[i].lineStart;
            chapter.byteEnd = (i + 1 < hits.size()) ? hits[i + 1].lineStart : size;
            chapters.push_back(std::move(chapter));
        }
    }

    std::vector<ChapterIndex> split;
    split.reserve(chapters.size());
    for (const ChapterIndex& chapter : chapters) {
        auto pieces = splitOne(utf8, size, chapter);
        for (auto& piece : pieces) {
            split.push_back(std::move(piece));
        }
    }
    if (split.size() > static_cast<std::size_t>(std::numeric_limits<quint32>::max())) {
        return std::unexpected(makeError(QStringLiteral("章节太多")));
    }
    for (std::size_t i = 0; i < split.size(); ++i) {
        split[i].index = static_cast<quint32>(i);
    }
    if (!chapterRangesValid(split, size)) {
        return std::unexpected(makeError(QStringLiteral("章节范围不连续")));
    }
    return split;
}

void pushParagraph(std::vector<Paragraph>& paragraphs, quint32 start, quint32 end, int codePoints, ParagraphKind kind)
{
    Paragraph paragraph;
    paragraph.byteStart = start;
    paragraph.byteEnd = end;
    paragraph.codePoints = codePoints;
    paragraph.kind = start == end ? ParagraphKind::Blank : kind;
    paragraphs.push_back(paragraph);
}

std::vector<Paragraph> splitParagraphs(const char* utf8, quint64 size)
{
    std::vector<Paragraph> paragraphs;
    if (utf8 == nullptr || size == 0) {
        paragraphs.push_back(Paragraph{});
        return paragraphs;
    }
    const char* end = utf8 + size;
    const char* cursor = utf8;
    while (cursor < end) {
        const Line line = takeLine(cursor, end);
        const char* contentBegin = line.begin;
        ParagraphKind kind = ParagraphKind::Body;
        if (contentBegin < line.contentEnd && *contentBegin == kHeadingMark) {
            kind = ParagraphKind::Heading;
            ++contentBegin;
        }
        if (kind == ParagraphKind::Heading && contentBegin == line.contentEnd) {
            cursor = line.next;
            if (line.next == line.begin) {
                break;
            }
            continue;
        }
        const quint64 lineStart = static_cast<quint64>(contentBegin - utf8);
        const quint64 contentEnd = static_cast<quint64>(line.contentEnd - utf8);
        if (contentEnd - lineStart > static_cast<quint64>(std::numeric_limits<quint32>::max())) {
            break;
        }
        const char* scan = contentBegin;
        quint64 pieceStart = lineStart;
        int points = 0;
        while (scan < line.contentEnd) {
            const int consumed = utf8SequenceLength(scan, line.contentEnd);
            if (consumed <= 0) {
                break;
            }
            scan += consumed;
            ++points;
            if (points >= kMaxParagraphCodePoints) {
                pushParagraph(paragraphs,
                              static_cast<quint32>(pieceStart),
                              static_cast<quint32>(scan - utf8),
                              points,
                              kind);
                pieceStart = static_cast<quint64>(scan - utf8);
                points = 0;
                kind = ParagraphKind::Body;
            }
        }
        if (points > 0 || pieceStart == lineStart) {
            pushParagraph(paragraphs,
                          static_cast<quint32>(pieceStart),
                          static_cast<quint32>(contentEnd),
                          points,
                          kind);
        }
        cursor = line.next;
        if (line.next == line.begin) {
            break;
        }
    }
    if (paragraphs.empty()) {
        paragraphs.push_back(Paragraph{});
    }
    return paragraphs;
}

ChapterContent makeChapterContent(const ChapterIndex& chapter, const char* body, quint64 bodySize)
{
    ChapterContent content;
    content.index = chapter.index;
    content.title = chapter.title;
    if (body == nullptr || chapter.byteEnd < chapter.byteStart || chapter.byteEnd > bodySize) {
        content.paragraphs = splitParagraphs(nullptr, 0);
        return content;
    }
    const quint64 length = chapter.byteEnd - chapter.byteStart;
    if (length > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        content.paragraphs = splitParagraphs(nullptr, 0);
        return content;
    }
    content.utf8 = QByteArray(body + chapter.byteStart, static_cast<qsizetype>(length));
    content.paragraphs = splitParagraphs(content.utf8.constData(), length);
    if (!chapter.synthetic && chapter.title != QStringLiteral("开头") && chapter.title != QStringLiteral("正文") &&
        !content.paragraphs.empty() && content.paragraphs.front().kind != ParagraphKind::Blank) {
        content.paragraphs.front().kind = ParagraphKind::Heading;
    }
    return content;
}

}  // namespace moyu
