#pragma once

#include "parser/TxtParser.h"

namespace moyu {

bool isSupportedBookPath(const QString& path);

// 文件对话框的过滤器。扩展名覆盖 TXT、EPUB、MOBI / AZW3、FB2、HTML、Markdown。
QString supportedBookFilter();

// TXT 交给 openTxtFile。其他格式先写成一份 UTF-8 缓存，再按同样的章节偏移打开。
// 这些格式的缓存命中只认 <token>.utf8，不把原文件当成 UTF-8 正文。
Result<TxtDocument> openBookFile(const QString& path,
                                 const QString& cacheDir,
                                 Database* database,
                                 quint32 chapterHint,
                                 quint32 byteHint,
                                 const std::atomic<bool>& cancel,
                                 const PreviewHandler& preview,
                                 OpenTimings* timings,
                                 const QString& chapterPattern = {});

}  // namespace moyu
