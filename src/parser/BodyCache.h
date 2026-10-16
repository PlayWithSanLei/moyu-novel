#pragma once

#include "parser/BookParser.h"

#include <atomic>
#include <vector>

namespace moyu {

struct TextChapter {
    QString title;
    quint8 level = 2;
    QByteArray utf8;
};

// 把各章拼成一份 UTF-8，写出 <token>.utf8。超长的章按段落拆开。
// 空章丢掉。每章末尾补一个换行，避免两章粘成一行。
Result<ParsedBook> writeBodyCache(Book book,
                                  const std::vector<TextChapter>& chapters,
                                  const QString& cacheDir,
                                  const QString& token,
                                  const std::atomic<bool>& cancel);

}  // namespace moyu
