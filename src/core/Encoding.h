#pragma once

#include "core/Types.h"
#include "core/Utf8.h"

#include <atomic>

namespace moyu {

struct DetectedEncoding {
    QString canonical;
    QString iconvName;
    quint64 bodyOffset = 0;
    NewlineCut cut = NewlineCut::Bytes;

    bool passthroughUtf8() const { return canonical == QLatin1String("UTF-8"); }
};

// 先看 BOM，再把前 64KB 交给 uchardet。认不出就报错，不按错误编码硬解。
// checkUtf8 为真时，判成 UTF-8 / ASCII 会把整个文件再验一遍。验失败就改用 GB18030。
// 缓存已经记过 UTF-8 时传 false，避免每次重开都重扫。
Result<DetectedEncoding> detectEncoding(const char* data, quint64 size, bool checkUtf8 = true);

// HTML 的 meta charset。只看文件开头。没有就返回空字符串。
QString htmlMetaCharset(const char* data, quint64 size);

// 把 charset 收成 detectEncoding 那一套名字。认不出就返回错误，调用方再去探测。
Result<DetectedEncoding> encodingFromDeclaredName(const QString& name);

// 测试用来确认某次打开没有再走转码。生产路径不要依赖这个计数。
quint64 transcodeCallCount();
void resetTranscodeCallCount();

// 不是 UTF-8 时分块转进缓存。非法字节写成 U+FFFD，不用 iconv 的 //IGNORE。
// \r\n 和单独的 \r 收成 \n。rename 之前做 F_FULLFSYNC，避免断电后留下全 0 的缓存。
// 缓存文件会一直留着，清理放到后面的步骤。
Result<void> transcodeToCacheFile(const char* data,
                                   quint64 size,
                                   const DetectedEncoding& encoding,
                                   const QString& destination,
                                   quint64 chunkTarget,
                                   const std::atomic<bool>& cancel);

// UTF-8 原文里如果有 \r，先收成 \n 再交给分章。换行已经是 \n 的文件不要走这里。
Result<void> normalizeNewlinesToCacheFile(const char* data,
                                           quint64 size,
                                           const QString& destination,
                                           const std::atomic<bool>& cancel);

}  // namespace moyu
