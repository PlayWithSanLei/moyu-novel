#include "core/TextSearch.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <functional>
#include <utility>

namespace moyu {
namespace {

quint64 alignToUtf8(const char* data, quint64 size, quint64 pos)
{
    while (pos < size && (static_cast<unsigned char>(data[pos]) & 0xC0U) == 0x80U) {
        ++pos;
    }
    return pos;
}

struct Chunk {
    quint64 from = 0;
    quint64 acceptEnd = 0;
    quint64 to = 0;
};

std::vector<Chunk> splitChunks(const char* data, quint64 size, quint64 chunkBytes, quint64 overlap)
{
    std::vector<Chunk> chunks;
    quint64 cursor = 0;
    while (cursor < size) {
        const quint64 from = alignToUtf8(data, size, cursor);
        if (from >= size) {
            break;
        }
        const quint64 acceptEnd = std::min(size, from + std::max<quint64>(chunkBytes, 1));
        quint64 to = std::min(size, acceptEnd + overlap);
        if (to < size) {
            const quint64 aligned = alignToUtf8(data, size, to);
            if (aligned > to && aligned <= size) {
                to = aligned;
            }
        }
        if (to <= from) {
            break;
        }
        chunks.push_back(Chunk{from, std::min(acceptEnd, to), to});
        if (acceptEnd <= cursor) {
            break;
        }
        cursor = acceptEnd;
    }
    return chunks;
}

std::vector<SearchHit> findInChunk(const char* data, const Chunk& chunk, const QByteArray& needle)
{
    std::vector<SearchHit> hits;
    if (chunk.to <= chunk.from || needle.isEmpty()) {
        return hits;
    }
    const char* begin = data + chunk.from;
    const char* end = data + chunk.to;
    const auto searcher = std::boyer_moore_horspool_searcher(needle.constData(), needle.constData() + needle.size());
    const char* cursor = begin;
    while (cursor < end) {
        const char* found = std::search(cursor, end, searcher);
        if (found == end) {
            break;
        }
        const auto offset = static_cast<quint64>(found - data);
        if (offset >= chunk.acceptEnd) {
            break;
        }
        hits.push_back(SearchHit{offset});
        cursor = found + 1;
    }
    return hits;
}

}  // namespace

Result<void> searchUtf8(const char* data,
                         quint64 size,
                         const QByteArray& needle,
                         quint64 chunkBytes,
                         const std::atomic<bool>& cancel,
                         const std::function<void(const std::vector<SearchHit>&)>& onBatch,
                         SearchTiming* timing)
{
    if (needle.isEmpty()) {
        return std::unexpected(makeError(QStringLiteral("搜索关键字是空的")));
    }
    if (data == nullptr || size == 0) {
        if (timing != nullptr) {
            *timing = {};
            timing->firstBatchMs = -1;
        }
        return {};
    }
    const quint64 overlap = needle.size() > 0 ? static_cast<quint64>(needle.size() - 1) : 0;
    const std::vector<Chunk> chunks = splitChunks(data, size, chunkBytes == 0 ? (1ULL << 18) : chunkBytes, overlap);

    QElapsedTimer timer;
    timer.start();
    QMutex mutex;
    SearchTiming local;
    local.firstBatchMs = -1;
    QThreadPool pool;
    pool.setMaxThreadCount(std::max(1, QThread::idealThreadCount()));

    for (const Chunk& chunk : chunks) {
        if (cancel.load()) {
            continue;
        }
        pool.start([&, chunk] {
            std::vector<SearchHit> hits;
            if (!cancel.load()) {
                hits = findInChunk(data, chunk, needle);
            }
            if (!hits.empty() && onBatch) {
                {
                    QMutexLocker lock(&mutex);
                    if (local.firstBatchMs < 0) {
                        local.firstBatchMs = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
                    }
                    local.hits += static_cast<quint64>(hits.size());
                }
                onBatch(hits);
            } else if (!hits.empty()) {
                QMutexLocker lock(&mutex);
                if (local.firstBatchMs < 0) {
                    local.firstBatchMs = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
                }
                local.hits += static_cast<quint64>(hits.size());
            }
        });
    }
    pool.waitForDone();
    local.totalMs = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
    local.cancelled = cancel.load();
    if (timing != nullptr) {
        QMutexLocker lock(&mutex);
        local.totalMs = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
        *timing = local;
    }
    return {};
}

}  // namespace moyu
