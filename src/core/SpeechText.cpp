#include "core/SpeechText.h"

#include "core/Utf8.h"

namespace moyu {
namespace {

bool sentenceEnd(QChar c)
{
    switch (c.unicode()) {
    case u'。':
    case u'！':
    case u'？':
    case u'；':
    case u'…':
    case u'!':
    case u'?':
    case u';':
        return true;
    default:
        return false;
    }
}

// 句末标点后面紧跟的引号、括号跟着上一句走。
bool closingMark(QChar c)
{
    switch (c.unicode()) {
    case u'”':
    case u'’':
    case u'」':
    case u'』':
    case u'）':
    case u'》':
    case u'"':
    case u'\'':
    case u')':
        return true;
    default:
        return false;
    }
}

bool speakable(const QString& text)
{
    for (const QChar c : text) {
        if (c.isLetterOrNumber()) {
            return true;
        }
    }
    return false;
}

QString cleaned(QString text)
{
    text.remove(QChar(u'\r'));
    text.remove(QChar(static_cast<char16_t>(static_cast<unsigned char>(kHeadingMark))));
    return text.trimmed();
}

int cutAfterSentenceEnd(const QString& text, int index)
{
    const int size = static_cast<int>(text.size());
    int cut = index + 1;
    while (cut < size && (closingMark(text.at(cut)) || sentenceEnd(text.at(cut)))) {
        ++cut;
    }
    return cut;
}

// fromByte 落在段中间时，从它所在那一句的句首读起。
int sentenceStart(const DecodedText& decoded, quint32 fromByte)
{
    const int size = static_cast<int>(decoded.text.size());
    int target = 0;
    while (target < size && decoded.utf16ToByte[static_cast<std::size_t>(target)] < fromByte) {
        ++target;
    }
    int start = 0;
    for (int i = 0; i < target; ++i) {
        if (sentenceEnd(decoded.text.at(i))) {
            const int cut = cutAfterSentenceEnd(decoded.text, i);
            if (cut <= target) {
                start = cut;
            }
        }
    }
    return start;
}

void pushSegment(std::vector<SpeechSegment>& out, quint32 chapter, const DecodedText& decoded, int begin, int end)
{
    if (end <= begin) {
        return;
    }
    QString text = cleaned(decoded.text.mid(begin, end - begin));
    if (!speakable(text)) {
        return;
    }
    SpeechSegment segment;
    segment.chapter = chapter;
    segment.byteStart = decoded.utf16ToByte[static_cast<std::size_t>(begin)];
    segment.byteEnd = decoded.utf16ToByte[static_cast<std::size_t>(end)];
    segment.text = std::move(text);
    out.push_back(std::move(segment));
}

}  // namespace

std::vector<SpeechSegment> speechSegments(const ChapterContent& content, quint32 fromByte)
{
    std::vector<SpeechSegment> out;
    const auto total = static_cast<quint64>(content.utf8.size());
    for (const Paragraph& paragraph : content.paragraphs) {
        if (paragraph.kind == ParagraphKind::Blank || paragraph.byteEnd <= fromByte ||
            paragraph.byteEnd <= paragraph.byteStart || paragraph.byteEnd > total) {
            continue;
        }
        const DecodedText decoded = decodeParagraph(content.utf8.constData() + paragraph.byteStart,
                                                    paragraph.byteEnd - paragraph.byteStart,
                                                    paragraph.byteStart);
        const int size = static_cast<int>(decoded.text.size());
        if (decoded.utf16ToByte.size() != static_cast<std::size_t>(size) + 1) {
            continue;
        }
        int begin = paragraph.byteStart < fromByte ? sentenceStart(decoded, fromByte) : 0;
        int lastCut = -1;
        for (int i = begin; i < size; ++i) {
            if (sentenceEnd(decoded.text.at(i))) {
                lastCut = cutAfterSentenceEnd(decoded.text, i);
            }
            if (i + 1 - begin < kMaxSpeechSegmentChars) {
                continue;
            }
            int cut = lastCut > begin ? lastCut : i + 1;
            // 硬切时不要把补充平面字符的两个码元拆开。
            if (cut < size && decoded.text.at(cut - 1).isHighSurrogate()) {
                ++cut;
            }
            pushSegment(out, content.index, decoded, begin, cut);
            begin = cut;
            lastCut = -1;
        }
        pushSegment(out, content.index, decoded, begin, size);
    }
    return out;
}

}  // namespace moyu
