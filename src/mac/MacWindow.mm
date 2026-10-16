#include "mac/MacWindow.h"

#include <QGuiApplication>
#include <QWidget>
#include <QWindow>

#undef slots
#undef signals
#undef emit
#import <AppKit/AppKit.h>

#include <utility>

// 接 kAEReopenApplication 的 Apple Event。整个进程只有一个，装上之后不释放。
@interface MoyuReopenHandler : NSObject {
@public
    std::function<void()> onReopen;
}
- (void)handleReopen:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)reply;
@end

@implementation MoyuReopenHandler
- (void)handleReopen:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)reply
{
    (void)event;
    (void)reply;
    if (onReopen) {
        onReopen();
    }
}
@end

namespace moyu {
namespace {

NSWindow* nativeWindow(QWidget* widget)
{
    // internalWinId() 是 protected。WA_WState_Created 为 false 时不能调用 winId()，
    // 否则会提前创建 NSWindow。已经创建之后，winId() 只返回现有的 NSView。
    // offscreen 之类的平台也会给出非 0 的 winId，但它不是 NSView。
    if (widget == nullptr || qGuiApp == nullptr || qGuiApp->platformName() != QLatin1String("cocoa")) {
        return nil;
    }
    if (!widget->testAttribute(Qt::WA_WState_Created)) {
        return nil;
    }
    const auto wid = static_cast<uintptr_t>(widget->winId());
    if (wid == 0) {
        return nil;
    }
    auto* view = reinterpret_cast<NSView*>(wid);
    return view.window;
}

}  // namespace

void MacWindow::setIgnoresMouseEvents(QWidget* widget, bool ignore)
{
    NSWindow* window = nativeWindow(widget);
    if (window == nil) {
        return;
    }
    window.ignoresMouseEvents = ignore ? YES : NO;
}

bool MacWindow::setHidesFromDock(QWidget* widget, bool hide, bool alwaysOnTop)
{
    if (widget == nullptr || qGuiApp == nullptr || qGuiApp->platformName() != QLatin1String("cocoa") || NSApp == nil) {
        return false;
    }
    const bool wasVisible = widget->isVisible();
    const NSApplicationActivationPolicy policy =
        hide ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular;
    [NSApp setActivationPolicy:policy];

    NSWindow* window = nativeWindow(widget);
    if (wasVisible && !widget->isVisible()) {
        widget->setAttribute(Qt::WA_ShowWithoutActivating, true);
        widget->show();
        widget->setAttribute(Qt::WA_ShowWithoutActivating, false);
        window = nativeWindow(widget);
    }
    if (window != nil && wasVisible && !window.isVisible) {
        [window orderFrontRegardless];
    }
    window = nativeWindow(widget);
    if (window != nil) {
        const bool floating = window.level == NSFloatingWindowLevel;
        if (floating != alwaysOnTop || (wasVisible && !window.isVisible)) {
            applyChrome(widget, alwaysOnTop);
            if (wasVisible && !window.isVisible) {
                [window orderFrontRegardless];
            }
        }
    }
    return window != nil;
}

void MacWindow::hideWithoutAnimation(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }
    // 先 orderOut，再让 Qt 把 visible 状态对齐。hide() 本身也是 orderOut，不会走最小化动画。
    NSWindow* window = nativeWindow(widget);
    if (window != nil) {
        [window orderOut:nil];
    }
    if (widget->isVisible()) {
        widget->hide();
    }
}

void MacWindow::showWithoutActivating(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }
    widget->setAttribute(Qt::WA_ShowWithoutActivating, true);
    if (!widget->isVisible()) {
        widget->show();
    }
    widget->setAttribute(Qt::WA_ShowWithoutActivating, false);
    NSWindow* window = nativeWindow(widget);
    if (window != nil) {
        [window orderFrontRegardless];
    }
}

void MacWindow::applyChrome(QWidget* widget, bool alwaysOnTop)
{
    NSWindow* window = nativeWindow(widget);
    if (window == nil) {
        return;
    }

    // WA_TranslucentBackground 对应的透明底色。Qt 重建窗口时会把它恢复成不透明。
    window.opaque = NO;
    window.backgroundColor = NSColor.clearColor;
    window.hasShadow = NO;
    // 背景拖动只走 startSystemMove 或我们自己的几何计算，不和 AppKit 抢。
    window.movableByWindowBackground = NO;

    NSWindowCollectionBehavior behavior = window.collectionBehavior;
    behavior |= NSWindowCollectionBehaviorCanJoinAllSpaces;
    behavior |= NSWindowCollectionBehaviorFullScreenAuxiliary;
    window.collectionBehavior = behavior;
    window.level = alwaysOnTop ? NSFloatingWindowLevel : NSNormalWindowLevel;
}

void MacWindow::installReopenHandler(std::function<void()> onReopen)
{
    if (qGuiApp == nullptr || qGuiApp->platformName() != QLatin1String("cocoa")) {
        return;
    }
    static MoyuReopenHandler* handler = [[MoyuReopenHandler alloc] init];
    handler->onReopen = std::move(onReopen);
    [[NSAppleEventManager sharedAppleEventManager] setEventHandler:handler
                                                       andSelector:@selector(handleReopen:withReplyEvent:)
                                                     forEventClass:kCoreEventClass
                                                        andEventID:kAEReopenApplication];
}

void MacWindow::makeKey(QWidget* widget)
{
    NSWindow* window = nativeWindow(widget);
    if (window == nil) {
        return;
    }
    // canBecomeMainWindow 保持 NO。阅读器不用主窗口身份，只需要能收键盘。
    // 应用还不是 active 时，makeKeyWindow 不会把 isKeyWindow 变成 YES。
    [NSApp activateIgnoringOtherApps:YES];
    [[NSRunningApplication currentApplication]
        activateWithOptions:NSApplicationActivateAllWindows | NSApplicationActivateIgnoringOtherApps];
    [window makeKeyWindow];
    if (NSView* content = window.contentView) {
        [window makeFirstResponder:content];
    }
}

bool MacWindow::postKeyDown(QWidget* widget, const char* characters, unsigned short keyCode)
{
    NSWindow* window = nativeWindow(widget);
    if (window == nil || characters == nullptr) {
        return false;
    }
    NSString* text = [NSString stringWithUTF8String:characters];
    if (text == nil) {
        return false;
    }
    // context: 参数已弃用，但这是 AppKit 里构造 keyDown 的入口。警告只在这一处压掉。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSEvent* event = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:NSProcessInfo.processInfo.systemUptime
                                   windowNumber:window.windowNumber
                                        context:nil
                                     characters:text
                    charactersIgnoringModifiers:text
                                      isARepeat:NO
                                        keyCode:keyCode];
#pragma clang diagnostic pop
    if (event == nil) {
        return false;
    }
    // 走 NSApp，Qt 装在 QNSApplication 上的分发才会把按键变成 QKeyEvent。
    [NSApp sendEvent:event];
    return true;
}

bool MacWindow::trySystemMove(QWidget* widget)
{
    if (nativeWindow(widget) == nil || widget->windowHandle() == nullptr) {
        return false;
    }
    NSEvent* event = NSApp.currentEvent;
    if (event == nil || event.type != NSEventTypeLeftMouseDown) {
        return false;
    }
    return widget->windowHandle()->startSystemMove();
}

WindowChromeState MacWindow::chromeState(QWidget* widget)
{
    WindowChromeState state;
    NSWindow* window = nativeWindow(widget);
    if (window == nil) {
        return state;
    }
    state.hasNativeWindow = true;
    state.alwaysOnTop = window.level == NSFloatingWindowLevel;
    state.joinsAllSpaces =
        (window.collectionBehavior & NSWindowCollectionBehaviorCanJoinAllSpaces) != 0;
    state.canBecomeKey = window.canBecomeKeyWindow == YES;
    state.isKeyWindow = window.isKeyWindow == YES;
    return state;
}

}  // namespace moyu
