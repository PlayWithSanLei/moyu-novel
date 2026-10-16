#pragma once

#include "parser/TxtParser.h"

namespace moyu {

Result<ParsedBook> parseEpub(const QString& path,
                             const QString& cacheDir,
                             const QString& token,
                             const std::atomic<bool>& cancel,
                             const PreviewHandler& preview);

}  // namespace moyu
