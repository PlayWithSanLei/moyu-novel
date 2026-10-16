#include "core/Utf8.h"

#include <QChar>

#include <algorithm>
#include <limits>

namespace moyu {
namespace {

struct Decoded {
    char32_t codePoint = 0xFFFD;
    int consumed = 1;
};

int continuation(const char* data, const char* end, int index)
{
    if (data + index >= end) {
        return -1;
    }
    const auto byte = static_cast<unsigned char>(data[index]);
    if ((byte & 0xC0) != 0x80) {
        return -1;
    }
    return byte & 0x3F;
}

Decoded decodeOne(const char* data, const char* end)
{
    if (data >= end) {
        return {0xFFFD, 0};
    }
    const auto lead = static_cast<unsigned char>(*data);
    if (lead <= 0x7F) {
        return {lead, 1};
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        const int a = continuation(data, end, 1);
        if (a < 0) {
            return {0xFFFD, 1};
        }
        return {(static_cast<char32_t>(lead & 0x1F) << 6) | static_cast<char32_t>(a), 2};
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        const int a = continuation(data, end, 1);
        const int b = continuation(data, end, 2);
        if (a < 0 || b < 0) {
            return {0xFFFD, 1};
        }
        // E0 后面必须是 A0–BF，否则是过长编码。ED 后面 A0–BF 是代理项。
        if ((lead == 0xE0 && a < 0x20) || (lead == 0xED && a >= 0x20)) {
            return {0xFFFD, 1};
        }
        const char32_t cp = (static_cast<char32_t>(lead & 0x0F) << 12) |
                             (static_cast<char32_t>(a) << 6) | static_cast<char32_t>(b);
        return {cp, 3};
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        const int a = continuation(data, end, 1);
        const int b = continuation(data, end, 2);
        const int c = continuation(data, end, 3);
        if (a < 0 || b < 0 || c < 0) {
            return {0xFFFD, 1};
        }
        if ((lead == 0xF0 && a < 0x10) || (lead == 0xF4 && a > 0x0F)) {
            return {0xFFFD, 1};
        }
        const char32_t cp = (static_cast<char32_t>(lead & 0x07) << 18) |
                             (static_cast<char32_t>(a) << 12) |
                             (static_cast<char32_t>(b) << 6) | static_cast<char32_t>(c);
        if (cp > 0x10FFFF) {
            return {0xFFFD, 1};
        }
        return {cp, 4};
    }
    return {0xFFFD, 1};
}

bool utf16Break(unsigned char first, unsigned char second, NewlineCut cut)
{
    const bool cr = cut == NewlineCut::Utf16Le ? (first == 0x0D && second == 0x00)
                                                : (first == 0x00 && second == 0x0D);
    const bool lf = cut == NewlineCut::Utf16Le ? (first == 0x0A && second == 0x00)
                                                : (first == 0x00 && second == 0x0A);
    return cr || lf;
}

quint64 lastNewline(const char* begin, quint64 window, NewlineCut cut)
{
    if (cut == NewlineCut::Bytes) {
        for (quint64 i = window; i > 0; --i) {
            const auto byte = static_cast<unsigned char>(begin[i - 1]);
            if (byte != 0x0A && byte != 0x0D) {
                continue;
            }
            if (byte == 0x0D && i < window && static_cast<unsigned char>(begin[i]) == 0x0A) {
                return i + 1;
            }
            return i;
        }
        return 0;
    }
    const quint64 aligned = window - (window % 2);
    for (quint64 i = aligned; i >= 2; i -= 2) {
        const auto first = static_cast<unsigned char>(begin[i - 2]);
        const auto second = static_cast<unsigned char>(begin[i - 1]);
        if (utf16Break(first, second, cut)) {
            return i;
        }
        if (i == 2) {
            break;
        }
    }
    return 0;
}

quint64 firstNewline(const char* begin, quint64 length, NewlineCut cut)
{
    if (cut == NewlineCut::Bytes) {
        for (quint64 i = 0; i < length; ++i) {
            const auto byte = static_cast<unsigned char>(begin[i]);
            if (byte == 0x0A) {
                return i + 1;
            }
            if (byte == 0x0D) {
                if (i + 1 < length && static_cast<unsigned char>(begin[i + 1]) == 0x0A) {
                    return i + 2;
                }
                return i + 1;
            }
        }
        return 0;
    }
    const quint64 aligned = length - (length % 2);
    for (quint64 i = 0; i + 1 < aligned; i += 2) {
        const auto first = static_cast<unsigned char>(begin[i]);
        const auto second = static_cast<unsigned char>(begin[i + 1]);
        if (utf16Break(first, second, cut)) {
            return i + 2;
        }
    }
    return 0;
}

quint64 alignCut(quint64 size, NewlineCut cut)
{
    if (cut == NewlineCut::Bytes || size < 2) {
        return size;
    }
    return size - (size % 2);
}

}  // namespace

int utf8SequenceLength(const char* data, const char* end)
{
    return decodeOne(data, end).consumed;
}

bool utf8WellFormed(const char* data, quint64 size)
{
    if (size == 0) {
        return true;
    }
    if (data == nullptr) {
        return false;
    }
    const char* cursor = data;
    const char* end = data + size;
    while (cursor < end) {
        const Decoded one = decodeOne(cursor, end);
        if (one.consumed <= 0) {
            return false;
        }
        // 非法序列也记成 U+FFFD，但只吃 1 个字节。真正的 U+FFFD 是 3 字节。
        if (one.codePoint == 0xFFFD && one.consumed == 1) {
            return false;
        }
        cursor += one.consumed;
    }
    return true;
}

int countCodePoints(const char* data, const char* end, int limit)
{
    int count = 0;
    const char* cursor = data;
    while (cursor < end && (limit < 0 || count < limit)) {
        const int consumed = decodeOne(cursor, end).consumed;
        if (consumed <= 0) {
            break;
        }
        cursor += consumed;
        ++count;
    }
    return count;
}

DecodedText decodeParagraph(const char* data, quint64 size, quint32 byteStart)
{
    DecodedText decoded;
    if (data == nullptr || size == 0) {
        decoded.utf16ToByte.push_back(byteStart);
        return decoded;
    }
    if (size > static_cast<quint64>(std::numeric_limits<quint32>::max() - byteStart)) {
        decoded.utf16ToByte.push_back(byteStart);
        return decoded;
    }
    const char* cursor = data;
    const char* end = data + size;
    decoded.text.reserve(static_cast<qsizetype>(size));
    decoded.utf16ToByte.reserve(static_cast<std::size_t>(size) + 1);
    while (cursor < end) {
        const auto offset = static_cast<quint32>(byteStart + static_cast<quint64>(cursor - data));
        const Decoded one = decodeOne(cursor, end);
        if (one.consumed <= 0) {
            break;
        }
        cursor += one.consumed;
        if (one.codePoint <= 0xFFFF) {
            decoded.text.append(QChar(static_cast<char16_t>(one.codePoint)));
            decoded.utf16ToByte.push_back(offset);
        } else {
            const char32_t value = one.codePoint - 0x10000;
            decoded.text.append(QChar(static_cast<char16_t>(0xD800 + (value >> 10))));
            decoded.text.append(QChar(static_cast<char16_t>(0xDC00 + (value & 0x3FF))));
            decoded.utf16ToByte.push_back(offset);
            decoded.utf16ToByte.push_back(offset);
        }
    }
    decoded.utf16ToByte.push_back(static_cast<quint32>(byteStart + size));
    return decoded;
}

quint64 findChunkEnd(const char* begin, const char* end, quint64 target, NewlineCut cut)
{
    if (begin == nullptr || end <= begin) {
        return 0;
    }
    const auto available = static_cast<quint64>(end - begin);
    if (target == 0) {
        target = 1;
    }
    // 单块上限。没有换行时不再把剩余文件全部吞进去，iconv 的输出缓冲也就不用按整本的 4 倍来。
    constexpr quint64 kMaxChunkBytes = 1 << 20;
    if (target > kMaxChunkBytes) {
        target = kMaxChunkBytes;
    }
    const quint64 limit = std::min(available, kMaxChunkBytes);
    quint64 window = std::min(target, available);
    if (cut != NewlineCut::Bytes) {
        window = alignCut(window, cut);
    }
    // 窗口比一个 UTF-16 码元还短时，从起点按码元向后找，但不超过上限。
    // 若从奇数偏移继续，会把「上」(U+4E0A，字节 0A 4E) 当成换行。
    if (cut != NewlineCut::Bytes && window < 2) {
        const quint64 ahead = firstNewline(begin, limit, cut);
        if (ahead > 0) {
            return ahead;
        }
        const quint64 capped = alignCut(limit, cut);
        return capped > 0 ? capped : std::min<quint64>(available, 1);
    }
    const quint64 inside = lastNewline(begin, window, cut);
    if (inside > 0) {
        return inside;
    }
    if (limit > window) {
        const quint64 ahead = firstNewline(begin + window, limit - window, cut);
        if (ahead > 0) {
            return window + ahead;
        }
    }
    const quint64 capped = alignCut(limit, cut);
    return capped > 0 ? capped : std::min<quint64>(available, 1);
}

}  // namespace moyu
