#pragma once

#include "core/Types.h"

#include <QString>

#include <vector>

namespace moyu {

enum class NewlineCut { Bytes, Utf16Le, Utf16Be };

struct DecodedText {
    QString text;
    // 每个 UTF-16 码元对应的章内字节偏移。补充平面的字符占两个码元，两个都指向同一个字节。
    // 末尾多一个哨兵，值是这段字节的结尾。
    std::vector<quint32> utf16ToByte;
};

// 非法序列返回 1：调用方把它换成 U+FFFD，并且只吃掉这一个字节。data == end 时返回 0。
int utf8SequenceLength(const char* data, const char* end);

// limit < 0 时数完。否则最多数 limit 个，用来判断「是不是已经超过 N」。
int countCodePoints(const char* data, const char* end, int limit);

DecodedText decodeParagraph(const char* data, quint64 size, quint32 byteStart);

// 严格的 UTF-8。非法字节、过长编码、半个序列都返回 false。U+FFFD 本身（EF BF BD）算合法。
bool utf8WellFormed(const char* data, quint64 size);

// 分块终点相对 begin 的偏移。\n 和 \r 都是换行。窗口里有换行就停在最后一个换行之后；
// 没有就在上限以内向后找。找不到也不超过 1MB，避免无换行文件把整本塞进一块。
// UTF-16 按 2 字节码元看，只在码元等于 0x000A 或 0x000D 时切开。
// 「上」(U+4E0A) 的字节里含有 0x0A，不能按字节搜。
quint64 findChunkEnd(const char* begin, const char* end, quint64 target, NewlineCut cut);

}  // namespace moyu
