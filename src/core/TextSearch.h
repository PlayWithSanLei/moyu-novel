#pragma once

#include "core/Types.h"

#include <QByteArray>

#include <atomic>
#include <functional>
#include <vector>

namespace moyu {

struct SearchHit {
    quint64 byteOffset = 0;
};

struct SearchTiming {
    // 还没有任何一批结果时是 -1。
    double firstBatchMs = -1;
    double totalMs = 0;
    quint64 hits = 0;
    bool cancelled = false;
};

// 在 UTF-8 字节上分块查找。块起点对齐到字符边界，相邻块重叠「关键字字节数 − 1」。
// 每块在线程池里跑，有结果就调用 onBatch，不用等全部结束。cancel 置位后不再开新块。
// needle 按字节匹配，调用方传入 UTF-8。空关键字算错误。
Result<void> searchUtf8(const char* data,
                         quint64 size,
                         const QByteArray& needle,
                         quint64 chunkBytes,
                         const std::atomic<bool>& cancel,
                         const std::function<void(const std::vector<SearchHit>&)>& onBatch,
                         SearchTiming* timing);

}  // namespace moyu
