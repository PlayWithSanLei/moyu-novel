#pragma once

#include "core/Types.h"

#include <atomic>

namespace moyu {

struct ParsedBook {
    Book book;
    std::vector<ChapterIndex> chapters;
    QString bodyPath;
    quint64 bodyOffset = 0;
};

class BookParser {
public:
    virtual ~BookParser() = default;

    virtual Result<ParsedBook> parseIndex(const QString& path, const std::atomic<bool>& cancel) = 0;
    virtual Result<ChapterContent> readChapter(const ParsedBook& book, quint32 index) = 0;
};

}  // namespace moyu
