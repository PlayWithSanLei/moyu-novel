#pragma once

#include <functional>

class QWidget;

namespace moyu {

struct WindowChromeState {
    bool hasNativeWindow = false;
    bool alwaysOnTop = false;
    bool joinsAllSpaces = false;
    bool canBecomeKey = false;
    bool isKeyWindow = false;
};

// 平台窗口的 level 和 collectionBehavior。
// 全部走 NSWindow，不改 Qt::WindowStaysOnTopHint：改 flags 会重建窗口并闪一下。
// Qt 6.8 的 QNSWindow 在无边框 + WA_TranslucentBackground 下 canBecomeKeyWindow 已经是 YES，
// 这里不再替换 isa。运行时改 Qt 窗口的类会和 KVO 的 isa 交换冲突，Qt 内部也有按类判断的逻辑。
class MacWindow {
public:
    // internalWinId 仍为 0 时直接返回，不调用 winId()，避免提前创建 NSWindow。
    static void applyChrome(QWidget* widget, bool alwaysOnTop);

    // 穿透只改 NSWindow.ignoresMouseEvents，不改 Qt 窗口 flags，避免重建窗口。
    static void setIgnoresMouseEvents(QWidget* widget, bool ignore);

    // 隐藏 Dock 会把激活策略改成 Accessory，系统菜单栏一起消失。
    // 有的系统版本会顺手把窗口 orderOut，或把 level 打回普通。这里核对，不对就再套一次 chrome。
    static bool setHidesFromDock(QWidget* widget, bool hide, bool alwaysOnTop);

    // 老板键：orderOut 没有最小化动画。恢复用 orderFrontRegardless，不调用 activate。
    static void hideWithoutAnimation(QWidget* widget);
    static void showWithoutActivating(QWidget* widget);

    // 点击后把键盘焦点交给本窗口。阅读器不需要成为 main window。
    static void makeKey(QWidget* widget);

    // 点 Dock 图标（或在访达里再次打开）时系统发 kAEReopenApplication。
    // 直接接管这个 Apple Event，只在用户明确重开时回调；Cmd+Tab 切回来不会触发。
    // 要在事件循环开始之后调用，AppKit 在 finishLaunching 里会装自己的处理器。
    static void installReopenHandler(std::function<void()> onReopen);

    // 当前 AppKit 事件确实是左键按下时，才调用 QWindow::startSystemMove()。
    // 合成的鼠标事件没有 NSEvent，硬调会走进系统的嵌套跟踪循环。
    // 返回 true 表示系统已经在跟踪拖动。
    static bool trySystemMove(QWidget* widget);

    // 经 NSApp 投递一次 keyDown，确认键盘能进到 Qt。测试使用，界面逻辑不调用。
    static bool postKeyDown(QWidget* widget, const char* characters, unsigned short keyCode);

    static WindowChromeState chromeState(QWidget* widget);
};

}  // namespace moyu
