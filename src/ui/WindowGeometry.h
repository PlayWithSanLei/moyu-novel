#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

#include <algorithm>

namespace moyu {

// 窗口可以缩得很小，但再小就点不中缩放边缘了。
inline constexpr QSize kMinWindowSize{240, 160};
inline constexpr int kResizeMarginPx = 6;

// macOS 上 QWindow::startSystemResize() 固定返回 false，边缘缩放在这里算。
// 按下时记下原始几何和鼠标位移，按被拖动的边改 left/top/right/bottom。
// 用包含端点的坐标，避免 QRect 的 width/height 在 setLeft 时互相改写。
inline QRect applyEdgeResize(const QRect& original, QPoint delta, Qt::Edges edges, QSize minimum)
{
    const int minWidth = std::max(1, minimum.width());
    const int minHeight = std::max(1, minimum.height());

    int left = original.left();
    int top = original.top();
    int right = original.right();
    int bottom = original.bottom();

    if (edges.testFlag(Qt::LeftEdge)) {
        left += delta.x();
    }
    if (edges.testFlag(Qt::RightEdge)) {
        right += delta.x();
    }
    if (edges.testFlag(Qt::TopEdge)) {
        top += delta.y();
    }
    if (edges.testFlag(Qt::BottomEdge)) {
        bottom += delta.y();
    }

    if (right - left + 1 < minWidth) {
        if (edges.testFlag(Qt::LeftEdge) && !edges.testFlag(Qt::RightEdge)) {
            left = right - minWidth + 1;
        } else {
            right = left + minWidth - 1;
        }
    }
    if (bottom - top + 1 < minHeight) {
        if (edges.testFlag(Qt::TopEdge) && !edges.testFlag(Qt::BottomEdge)) {
            top = bottom - minHeight + 1;
        } else {
            bottom = top + minHeight - 1;
        }
    }

    return QRect(left, top, right - left + 1, bottom - top + 1);
}

// 左右边缘同时命中只可能出现在比 2*margin 还窄的窗口上。最小宽度远大于这个值，
// 所以同一轴的两边用 if / else，角上则保留水平边和垂直边的组合。
inline Qt::Edges edgesAt(QPoint pos, QSize windowSize, int margin)
{
    Qt::Edges edges;
    if (margin < 1 || windowSize.isEmpty()) {
        return edges;
    }
    if (pos.x() < margin) {
        edges |= Qt::LeftEdge;
    } else if (pos.x() >= windowSize.width() - margin) {
        edges |= Qt::RightEdge;
    }
    if (pos.y() < margin) {
        edges |= Qt::TopEdge;
    } else if (pos.y() >= windowSize.height() - margin) {
        edges |= Qt::BottomEdge;
    }
    return edges;
}

inline Qt::CursorShape cursorForEdges(Qt::Edges edges)
{
    const bool left = edges.testFlag(Qt::LeftEdge);
    const bool right = edges.testFlag(Qt::RightEdge);
    const bool top = edges.testFlag(Qt::TopEdge);
    const bool bottom = edges.testFlag(Qt::BottomEdge);
    const bool horizontal = left || right;
    const bool vertical = top || bottom;
    if (horizontal && vertical) {
        // SizeFDiagCursor 是「\」（左上、右下），SizeBDiagCursor 是「/」。
        const bool backslash = (left && top) || (right && bottom);
        return backslash ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
    }
    if (horizontal) {
        return Qt::SizeHorCursor;
    }
    if (vertical) {
        return Qt::SizeVerCursor;
    }
    return Qt::ArrowCursor;
}

}  // namespace moyu
