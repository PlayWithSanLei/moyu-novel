#pragma once

#include "parser/TxtParser.h"

namespace moyu {

// MOBI、AZW、AZW3、PRC 都走这里。带 DRM 的只返回错误，不解密。
Result<ParsedBook> parseMobi(const QString& path,
                             const QString& cacheDir,
                             const QString& token,
                             const std::atomic<bool>& cancel,
                             const PreviewHandler& preview);

}  // namespace moyu
