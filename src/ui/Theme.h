#pragma once

#include <QColor>

namespace moyu {

// 不用 setWindowOpacity：背景和文字的 alpha 以后要分开调。
// 未开穿透时整窗 alpha 至少为 1。macOS 会让点击穿过全透明像素。
inline const QColor kPageColor{28, 28, 30, 210};
inline const QColor kInkColor{255, 255, 255, 230};

}  // namespace moyu
