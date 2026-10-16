#pragma once

#include "parser/BodyCache.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <atomic>
#include <vector>

namespace moyu {

// 加载线程栈只有 512KB。超过这个深度就不再交给会递归的 XML 解析器。
inline constexpr int kMaxMarkupDepth = 256;

struct MarkupText {
    QString text;
    QString firstHeading;
};

// 同一个 XHTML 按目录锚点切开。anchor 为空的是第一个锚点之前的前言。
struct MarkupSlice {
    QString anchor;
    QString text;
};

// 元素嵌套超过 limit 时返回 true。自己用循环数，不递归。
bool markupDeeperThan(const char* data, quint64 size, int limit);

// XHTML 必须能被 pugixml 解析。失败时调用方可以改用宽松的标签剥离。
// 嵌套超过 kMaxMarkupDepth 时返回「文件结构异常」。
Result<MarkupText> xhtmlToPlain(const char* data, quint64 size);

// anchors 里的 id 会新开一片。没有锚点时只有一片，anchor 为空。
Result<std::vector<MarkupSlice>> xhtmlToAnchoredSlices(const char* data, quint64 size, const QStringList& anchors);

// 不要求标签闭合。script / style 整段丢掉，块级标签和 br 变成换行。
// 自闭合的 script / style 不进入丢弃状态。cancel 在长循环里会停下来。
MarkupText htmlToPlainLenient(const char* data, quint64 size, const std::atomic<bool>* cancel = nullptr);

// 行首是标题标记的地方新开一章。标记本身不进正文。
std::vector<TextChapter> chaptersFromMarkedText(const QString& text, const QString& bookTitle);

QString firstMarkedHeading(const QString& text);

}  // namespace moyu
