#pragma once

namespace moyu {

// 穿透只能在「切换穿透」的全局热键已经注册、菜单栏图标也在的时候打开。
// 热键被占用或图标没出来时保持关闭，否则用户点不回这个窗口。
enum class ClickThroughDecision {
    TurnOn,
    TurnOff,
    StayOff,
};

inline ClickThroughDecision decideClickThrough(bool wantOn, bool hotkeyReady, bool statusItemReady)
{
    if (!wantOn) {
        return ClickThroughDecision::TurnOff;
    }
    if (!hotkeyReady || !statusItemReady) {
        return ClickThroughDecision::StayOff;
    }
    return ClickThroughDecision::TurnOn;
}

// 不开穿透时，背景至少留 1/255。全透明像素在 macOS 上会让点击直接穿过去。
inline int opacityAlpha(int percent, bool clickThrough)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    int alpha = percent * 255 / 100;
    if (!clickThrough && alpha < 1) {
        alpha = 1;
    }
    return alpha;
}

}  // namespace moyu
