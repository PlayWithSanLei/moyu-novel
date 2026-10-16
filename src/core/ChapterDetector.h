#pragma once

#include "core/Types.h"

#include <atomic>

namespace moyu {

// 内置规则里命中最多、又没密到像正文的那一套。
// customPattern 非空时先试它。命中数量合理就用它，否则仍用内置规则。
// 标题行去掉首尾空白后不超过 40 个字。正则无效时返回「章节正则无效」。
// 中文或英文胜出时，楔子、序章、后记、番外这些辅助标题也留在目录里。
// 没有任何标题时整本是一章，超过 10 万字再按段落拆成伪章节。
// \n、\r\n、单独的 \r 都是换行。
Result<std::vector<ChapterIndex>> detectChapters(const char* utf8,
                                                  quint64 size,
                                                  const std::atomic<bool>* cancel,
                                                  const QString& customPattern = {});

bool chapterRangesValid(const std::vector<ChapterIndex>& chapters, quint64 bodySize);

// 按换行切开。一行超过 kMaxParagraphCodePoints 再按码点拆开。byteEnd 不含换行。
std::vector<Paragraph> splitParagraphs(const char* utf8, quint64 size);

ChapterContent makeChapterContent(const ChapterIndex& chapter, const char* body, quint64 bodySize);

}  // namespace moyu
