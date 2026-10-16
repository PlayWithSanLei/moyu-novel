#pragma once

#include "parser/TxtParser.h"

namespace moyu {

Result<ParsedBook> parseHtml(const QString& path,
                             const QString& cacheDir,
                             const QString& token,
                             const std::atomic<bool>& cancel,
                             const PreviewHandler& preview);

Result<ParsedBook> parseMarkdown(const QString& path,
                                 const QString& cacheDir,
                                 const QString& token,
                                 const std::atomic<bool>& cancel,
                                 const PreviewHandler& preview);

Result<ParsedBook> parseFb2(const QString& path,
                            const QString& cacheDir,
                            const QString& token,
                            const std::atomic<bool>& cancel,
                            const PreviewHandler& preview);

}  // namespace moyu
