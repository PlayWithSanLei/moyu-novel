#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

namespace moyu {

enum class BookFormat { Txt, Epub, Mobi, Fb2, Html, Markdown };

struct ParseError {
    QString message;
    bool cancelled = false;
};

template <typename T>
using Result = std::expected<T, ParseError>;

struct Book {
    qint64 id = 0;
    QString filePath;
    QString fingerprint;
    QString title;
    QString author;
    BookFormat format = BookFormat::Txt;
    QString sourceEncoding;
    quint64 totalBytes = 0;
    qint64 lastReadAt = 0;
};

struct ChapterIndex {
    quint32 index = 0;
    QString title;
    quint8 level = 2;
    quint64 byteStart = 0;
    quint64 byteEnd = 0;
    bool synthetic = false;
};

struct ReadingProgress {
    qint64 bookId = 0;
    quint32 chapterIndex = 0;
    quint32 byteOffsetInChapter = 0;
    qint64 updatedAt = 0;
};

struct Bookmark {
    qint64 id = 0;
    qint64 bookId = 0;
    quint32 chapterIndex = 0;
    quint32 byteOffsetInChapter = 0;
    QString preview;
    qint64 createdAt = 0;
};

// 书架一行。percent 是 0 到 100 的整数，按最后阅读时间排序由查询负责。
struct ShelfEntry {
    qint64 id = 0;
    QString title;
    QString filePath;
    QString fingerprint;
    qint64 lastReadAt = 0;
    int percent = 0;
};

enum class ParagraphKind { Body, Heading, Blank };

// 行首这个字节表示标题。splitParagraphs 会把它拿掉，正文里不显示。
// 用一个不会出现在正常 UTF-8 正文里的控制字节，避免再做一张标题偏移表。
inline constexpr char kHeadingMark = '\x1E';

// 字节偏移相对本章 UTF-8，不含行尾的 \n。展示时再去掉 \r。
struct Paragraph {
    quint32 byteStart = 0;
    quint32 byteEnd = 0;
    int codePoints = 0;
    ParagraphKind kind = ParagraphKind::Body;
};

struct ChapterContent {
    quint32 index = 0;
    QString title;
    QByteArray utf8;
    std::vector<Paragraph> paragraphs;
};

struct StoredBook {
    Book book;
    std::vector<ChapterIndex> chapters;
    std::optional<ReadingProgress> progress;
};

inline ParseError makeError(const QString& message)
{
    return ParseError{message, false};
}

inline ParseError cancelledError()
{
    return ParseError{QStringLiteral("已取消"), true};
}

// 单章超过这个码点数就按约 1 万字拆开。没有标题的 TXT 整本会落成一章。
inline constexpr int kMaxChapterCodePoints = 100000;
inline constexpr int kSyntheticChapterCodePoints = 10000;
// 再长的段会在界面线程上做 QTextLayout。先按这个长度切开。
inline constexpr int kMaxParagraphCodePoints = 2000;

}  // namespace moyu
