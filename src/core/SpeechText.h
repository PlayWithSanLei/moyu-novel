#pragma once

#include "core/Types.h"

#include <QString>

#include <vector>

namespace moyu {

// 朗读的一段。字节偏移相对本章 UTF-8，和 Paragraph 一样。
struct SpeechSegment {
    quint32 chapter = 0;
    quint32 byteStart = 0;
    quint32 byteEnd = 0;
    QString text;
};

// 一次合成请求的字数上限。超过就在句末标点处切开，单段不会合成太久。
inline constexpr int kMaxSpeechSegmentChars = 300;

// 从 fromByte 所在的那一句起，把本章切成朗读段。空行和只有符号的行跳过。
std::vector<SpeechSegment> speechSegments(const ChapterContent& content, quint32 fromByte);

}  // namespace moyu
