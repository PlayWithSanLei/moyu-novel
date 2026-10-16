#include "app/BookSession.h"
#include "app/Hotkeys.h"
#include "app/StealthPolicy.h"
#include "core/ChapterDetector.h"
#include "core/Database.h"
#include "mac/GlobalHotkeys.h"
#include "mac/MacWindow.h"
#include "ui/MainWindow.h"
#include "ui/ReadingStyle.h"
#include "ui/ReadingTools.h"
#include "ui/ShelfDialog.h"
#include "ui/StatusMenu.h"
#include "ui/WindowGeometry.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QDate>
#include <QDateTime>
#include <QTableWidget>
#include <QTest>
#include <QTime>
#include <QTimer>
#include <QUrl>

namespace {

// 按键计数放在测试里。正式界面不留 keyPressCount 这类钩子。
class KeyProbe : public QObject {
public:
    explicit KeyProbe(QWidget* root)
        : m_root(root)
    {
        qApp->installEventFilter(this);
    }

    ~KeyProbe() override { qApp->removeEventFilter(this); }

    int presses = 0;
    int lastKey = 0;
    QString lastText;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress && m_root != nullptr) {
            auto* widget = qobject_cast<QWidget*>(watched);
            if (widget != nullptr && (widget == m_root || m_root->isAncestorOf(widget))) {
                const auto* key = static_cast<const QKeyEvent*>(event);
                ++presses;
                lastKey = key->key();
                lastText = key->text();
            }
        }
        return false;
    }

private:
    QWidget* m_root = nullptr;
};

}  // namespace

class WindowTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void leftEdgeGrowsAndClamps();
    void rightEdgeGrows();
    void topEdgeClampsHeight();
    void bottomRightCorner();
    void edgesAndCursors();
    void applyChromeBeforeNativeWindowIsSafe();
    void framelessTranslucentAndOpaqueEnough();
    void hoverCursorMatchesEdge();
    void dragRightEdgeResizesAndClamps();
    void dragClientAreaMovesWhenSystemMoveDeclines();
    void hideShowKeepsAlwaysOnTop();
    void clickThenKeyReachesWindow();
    void zeroOpacityKeepsClickableAlpha();
    void idleHasNoActiveTimer();
    void clickThroughStartsNoTimer();
    void closeShortcutHidesWindow();
    void bossHideDoesNotWaitForDatabase();
    void blurHideIgnoresPageKeys();
    void accessoryKeepsWindowAndOnTop();
    void statusMenuHasEscapeActions();
    void reservedHotkeysAndClickThroughGate();
    void duplicateHotkeyRegistrationFails();
    void backStackReturnsToThePreviousPlace();
    void bossHidePausesAutoScroll();
    void searchShortcutOpensPanel();
    void readerBackgroundMatchesResizeMargin();
    void shelfListsTitlePercentAndTime();
    void localDropKeepsEveryPath();
    void presetBackgroundsDiffer();
};

void WindowTest::leftEdgeGrowsAndClamps()
{
    const QRect original(100, 80, 300, 200);
    const QRect grown = moyu::applyEdgeResize(original, QPoint(-50, 0), Qt::LeftEdge, moyu::kMinWindowSize);
    QCOMPARE(grown, QRect(50, 80, 350, 200));

    const QRect tiny(0, 0, 240, 160);
    const QRect clamped = moyu::applyEdgeResize(tiny, QPoint(80, 0), Qt::LeftEdge, moyu::kMinWindowSize);
    QCOMPARE(clamped, QRect(0, 0, 240, 160));
}

void WindowTest::rightEdgeGrows()
{
    const QRect original(10, 20, 240, 160);
    const QRect grown = moyu::applyEdgeResize(original, QPoint(30, 0), Qt::RightEdge, moyu::kMinWindowSize);
    QCOMPARE(grown, QRect(10, 20, 270, 160));

    const QRect clamped =
        moyu::applyEdgeResize(original, QPoint(-100, 0), Qt::RightEdge, moyu::kMinWindowSize);
    QCOMPARE(clamped, QRect(10, 20, 240, 160));
}

void WindowTest::topEdgeClampsHeight()
{
    const QRect original(0, 0, 300, 200);
    const QRect clamped = moyu::applyEdgeResize(original, QPoint(0, 100), Qt::TopEdge, moyu::kMinWindowSize);
    QCOMPARE(clamped, QRect(0, 40, 300, 160));
}

void WindowTest::bottomRightCorner()
{
    const QRect original(100, 80, 300, 200);
    const QRect grown =
        moyu::applyEdgeResize(original, QPoint(-10, 15), Qt::LeftEdge | Qt::BottomEdge, moyu::kMinWindowSize);
    QCOMPARE(grown, QRect(90, 80, 310, 215));
}

void WindowTest::edgesAndCursors()
{
    const QSize size(300, 200);
    QCOMPARE(moyu::edgesAt(QPoint(0, 100), size, moyu::kResizeMarginPx), Qt::LeftEdge);
    QCOMPARE(moyu::edgesAt(QPoint(3, 3), size, moyu::kResizeMarginPx), Qt::LeftEdge | Qt::TopEdge);
    QCOMPARE(moyu::edgesAt(QPoint(297, 100), size, moyu::kResizeMarginPx), Qt::RightEdge);
    QCOMPARE(moyu::edgesAt(QPoint(297, 197), size, moyu::kResizeMarginPx), Qt::RightEdge | Qt::BottomEdge);
    QCOMPARE(moyu::edgesAt(QPoint(150, 100), size, moyu::kResizeMarginPx), Qt::Edges{});
    QCOMPARE(moyu::edgesAt(QPoint(293, 100), size, moyu::kResizeMarginPx), Qt::Edges{});

    QCOMPARE(moyu::cursorForEdges(Qt::LeftEdge), Qt::SizeHorCursor);
    QCOMPARE(moyu::cursorForEdges(Qt::TopEdge), Qt::SizeVerCursor);
    QCOMPARE(moyu::cursorForEdges(Qt::LeftEdge | Qt::TopEdge), Qt::SizeFDiagCursor);
    QCOMPARE(moyu::cursorForEdges(Qt::RightEdge | Qt::BottomEdge), Qt::SizeFDiagCursor);
    QCOMPARE(moyu::cursorForEdges(Qt::RightEdge | Qt::TopEdge), Qt::SizeBDiagCursor);
    QCOMPARE(moyu::cursorForEdges(Qt::LeftEdge | Qt::BottomEdge), Qt::SizeBDiagCursor);
    QCOMPARE(moyu::cursorForEdges({}), Qt::ArrowCursor);
}

void WindowTest::applyChromeBeforeNativeWindowIsSafe()
{
    moyu::MainWindow window;
    moyu::MacWindow::applyChrome(&window, true);
    const moyu::WindowChromeState state = moyu::MacWindow::chromeState(&window);
    QVERIFY(!state.hasNativeWindow);
    QVERIFY(!window.alwaysOnTop());
}

void WindowTest::framelessTranslucentAndOpaqueEnough()
{
    moyu::MainWindow window;
    QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(window.windowFlags().testFlag(Qt::NoDropShadowWindowHint));
    QVERIFY(window.testAttribute(Qt::WA_TranslucentBackground));
    QCOMPARE(window.windowOpacity(), 1.0);
    QCOMPARE(window.minimumSize(), moyu::kMinWindowSize);

    window.resize(300, 200);
    window.show();
    QImage image(window.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    window.render(&image);
    QVERIFY(image.pixelColor(0, 0).alpha() >= 1);
    QVERIFY(image.pixelColor(image.width() - 1, image.height() - 1).alpha() >= 1);
    QVERIFY(image.pixelColor(image.width() / 2, image.height() / 2).alpha() >= 1);
}

void WindowTest::hoverCursorMatchesEdge()
{
    // 不用 QTest::mouseMove。没按键时它会调用 QCursor::setPos，把用户的光标拽走，
    // 真正的窗口随后还会收到 leave，光标被清掉。这里直接把 MouseMove 交给窗口。
    moyu::MainWindow window;
    window.resize(300, 220);
    const auto moveTo = [&window](QPoint pos) {
        QMouseEvent event(QEvent::MouseMove,
                          QPointF(pos),
                          QPointF(pos),
                          Qt::NoButton,
                          Qt::NoButton,
                          Qt::NoModifier);
        QApplication::sendEvent(&window, &event);
    };
    moveTo(QPoint(1, 110));
    QCOMPARE(window.cursor().shape(), Qt::SizeHorCursor);
    moveTo(QPoint(1, 1));
    QCOMPARE(window.cursor().shape(), Qt::SizeFDiagCursor);
    moveTo(QPoint(298, 1));
    QCOMPARE(window.cursor().shape(), Qt::SizeBDiagCursor);
    moveTo(QPoint(150, 110));
    QCOMPARE(window.cursor().shape(), Qt::ArrowCursor);
}

void WindowTest::dragRightEdgeResizesAndClamps()
{
    moyu::MainWindow window;
    window.resize(300, 200);
    window.show();
    const QRect before = window.geometry();
    const QPoint start(window.width() - 2, window.height() / 2);
    const QPoint end = start + QPoint(40, 0);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&window, end);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, end);
    QCOMPARE(window.width(), before.width() + 40);
    QCOMPARE(window.height(), before.height());
    QCOMPARE(window.pos(), before.topLeft());

    window.resize(moyu::kMinWindowSize);
    const QPoint shrinkStart(window.width() - 2, 80);
    const QPoint shrinkEnd = shrinkStart + QPoint(-80, 0);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, shrinkStart);
    QTest::mouseMove(&window, shrinkEnd);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, shrinkEnd);
    QCOMPARE(window.width(), moyu::kMinWindowSize.width());
    QCOMPARE(window.height(), moyu::kMinWindowSize.height());
}

void WindowTest::dragClientAreaMovesWhenSystemMoveDeclines()
{
    moyu::MainWindow window;
    window.resize(300, 200);
    window.show();
    const QPoint before = window.pos();
    const QSize size = window.size();
    const QPoint start(120, 90);
    const QPoint end = start + QPoint(35, -15);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&window, end);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, end);
    QCOMPARE(window.pos(), before + QPoint(35, -15));
    QCOMPARE(window.size(), size);
}

void WindowTest::hideShowKeepsAlwaysOnTop()
{
    moyu::MainWindow window;
    window.setAlwaysOnTop(true);
    window.show();
    QTest::qWait(50);
    window.hide();
    QTest::qWait(20);
    window.show();
    QTest::qWait(50);

    QVERIFY(window.alwaysOnTop());
    const moyu::WindowChromeState state = moyu::MacWindow::chromeState(&window);
    if (!state.hasNativeWindow) {
        QSKIP("当前平台没有 NSWindow，跳过 level 检查。设置 QT_QPA_PLATFORM=cocoa 可测原生窗口。");
    }
    QVERIFY2(state.alwaysOnTop, "hide/show 之后应重新套用 NSFloatingWindowLevel");
    QVERIFY2(state.joinsAllSpaces, "应保持 CanJoinAllSpaces");
    QVERIFY2(state.canBecomeKey, "无边框窗口应能成为 key window");

    window.setAlwaysOnTop(false);
    QTest::qWait(20);
    const moyu::WindowChromeState lowered = moyu::MacWindow::chromeState(&window);
    QVERIFY(!window.alwaysOnTop());
    QVERIFY(lowered.hasNativeWindow);
    QVERIFY(!lowered.alwaysOnTop);
    QVERIFY(lowered.joinsAllSpaces);
}

void WindowTest::clickThenKeyReachesWindow()
{
    moyu::MainWindow window;
    window.resize(320, 200);
    window.show();
    QTest::qWait(50);
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(160, 100));
    QTest::qWait(20);

    KeyProbe probe(&window);
    const moyu::WindowChromeState state = moyu::MacWindow::chromeState(&window);
    if (state.hasNativeWindow) {
        QVERIFY2(state.canBecomeKey, "canBecomeKeyWindow 应为 YES");
        QVERIFY2(state.isKeyWindow, "点击后无边框窗口应为 key window");
        // 不走 QTest::keyClick。QTest 直接构造 QKeyEvent，看不出 NSWindow 拒收键盘。
        // kVK_ANSI_K = 40。
        probe.presses = 0;
        QVERIFY2(moyu::MacWindow::postKeyDown(&window, "k", 40), "NSApp 未收下 keyDown");
        QApplication::processEvents();
        QCOMPARE(probe.presses, 1);
        QVERIFY2(probe.lastKey == Qt::Key_K || probe.lastText == QLatin1String("k"),
                 "keyDown 没有到达窗口");
        return;
    }
    if (QGuiApplication::platformName() == QLatin1String("cocoa")) {
        QFAIL("cocoa 平台上 show() 之后应该有 NSWindow");
    }

    probe.presses = 0;
    QTest::keyClick(&window, Qt::Key_K);
    QCOMPARE(probe.presses, 1);
    QCOMPARE(probe.lastKey, static_cast<int>(Qt::Key_K));
}

void WindowTest::zeroOpacityKeepsClickableAlpha()
{
    moyu::ReadingStyle style;
    style.backgroundOpacityPercent = 0;
    QCOMPARE(moyu::styledBackground(style, false).alpha(), 1);
    QCOMPARE(moyu::styledBackground(style, true).alpha(), 0);
    QCOMPARE(moyu::opacityAlpha(0, false), 1);
    QCOMPARE(moyu::opacityAlpha(100, false), 255);

    moyu::MainWindow window;
    window.resize(240, 160);
    window.setBackgroundOpacityPercent(0);
    window.show();
    QApplication::processEvents();
    QCOMPARE(window.windowOpacity(), 1.0);
    QImage image(window.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    window.render(&image);
    QCOMPARE(image.pixelColor(4, 4).alpha(), 1);

    window.setClickThrough(true);
    image.fill(Qt::transparent);
    window.render(&image);
    QCOMPARE(image.pixelColor(4, 4).alpha(), 0);
    QCOMPARE(window.windowOpacity(), 1.0);
}

void WindowTest::idleHasNoActiveTimer()
{
    moyu::MainWindow window;
    const auto timers = window.findChildren<QTimer*>();
    QVERIFY(!timers.isEmpty());
    for (QTimer* timer : timers) {
        QVERIFY2(!timer->isActive(), "空闲时不应该有正在跑的定时器");
    }
}

void WindowTest::clickThroughStartsNoTimer()
{
    // 移出变淡已经去掉。穿透开着、鼠标进出窗口，颜色都不变，也不起定时器。
    moyu::MainWindow window;
    window.resize(300, 200);
    window.show();
    QApplication::processEvents();
    QImage before(window.size(), QImage::Format_ARGB32);
    before.fill(Qt::transparent);
    window.render(&before);

    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&window, &leave);
    QImage after(window.size(), QImage::Format_ARGB32);
    after.fill(Qt::transparent);
    window.render(&after);
    QCOMPARE(after.pixelColor(20, 20), before.pixelColor(20, 20));

    window.setClickThrough(true);
    for (QTimer* timer : window.findChildren<QTimer*>()) {
        QVERIFY2(!timer->isActive(), "穿透打开后不应该有轮询定时器");
    }
    window.setClickThrough(false);
}

void WindowTest::closeShortcutHidesWindow()
{
    moyu::MainWindow window;
    window.resize(300, 200);
    window.show();
    QApplication::processEvents();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(150, 100));
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QSignalSpy hidden(&window, &moyu::MainWindow::bossHidden);
    QWidget* target = window.focusWidget() != nullptr ? window.focusWidget() : static_cast<QWidget*>(&window);
    QTest::keyClick(target, Qt::Key_W, Qt::ControlModifier);
    QApplication::processEvents();
    QCOMPARE(hidden.count(), 1);
    QVERIFY(window.isBossHidden());
    // Command+W 之后进程还在，showForBoss（Dock 重开走的就是它）能把窗口叫回来。
    window.showForBoss();
    QApplication::processEvents();
    QVERIFY(!window.isBossHidden());
    QVERIFY(window.isVisible());
}

void WindowTest::bossHideDoesNotWaitForDatabase()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    moyu::Database database(dir.filePath(QStringLiteral("moyu.db")));
    QVERIFY(database.startAndWait());
    moyu::BookSession session(&database, dir.filePath(QStringLiteral("cache")));
    moyu::MainWindow window;
    window.resize(320, 220);
    window.show();
    QApplication::processEvents();

    bool visibleAtSave = true;
    QObject::connect(&window, &moyu::MainWindow::bossHidden, &window, [&] {
        visibleAtSave = window.isVisible();
        session.flushNow();
    }, Qt::DirectConnection);

    const QString connection = QStringLiteral("boss-hide-hold");
    QSqlDatabase hold = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    hold.setDatabaseName(dir.filePath(QStringLiteral("moyu.db")));
    QVERIFY(hold.open());
    qint64 elapsed = 0;
    {
        QSqlQuery lock(hold);
        QVERIFY(lock.exec(QStringLiteral("BEGIN IMMEDIATE")));
        QElapsedTimer timer;
        timer.start();
        window.hideForBoss();
        elapsed = timer.elapsed();
        QVERIFY(lock.exec(QStringLiteral("ROLLBACK")));
        lock.finish();
    }
    hold.close();
    hold = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);

    QVERIFY(!visibleAtSave);
    QVERIFY(!window.isVisible());
    QVERIFY(window.isBossHidden());
    // 藏窗口本身要几十毫秒。数据库被别的连接锁住时会到秒级，这里只要没去等那把锁。
    QVERIFY2(elapsed < 150, qPrintable(QStringLiteral("隐藏等了 %1 ms").arg(elapsed)));
    QVERIFY(!window.turnPageFromHotkey(true));
    QVERIFY(!window.isVisible());
    window.showForBoss();
    QVERIFY(window.isVisible());
    QVERIFY(!window.isBossHidden());
}

void WindowTest::blurHideIgnoresPageKeys()
{
    moyu::MainWindow window;
    window.resize(280, 180);
    window.show();
    window.setHideOnBlur(true);
    QEvent event(QEvent::WindowDeactivate);
    QApplication::sendEvent(&window, &event);
    QVERIFY(window.isBossHidden());
    QVERIFY(!window.isVisible());
    QVERIFY(!window.turnPageFromHotkey(false));
    QVERIFY(!window.isVisible());
}

void WindowTest::accessoryKeepsWindowAndOnTop()
{
    moyu::MainWindow window;
    window.setAlwaysOnTop(true);
    window.resize(320, 200);
    window.show();
    QTest::qWait(50);
    const moyu::WindowChromeState before = moyu::MacWindow::chromeState(&window);
    if (!before.hasNativeWindow) {
        QSKIP("当前平台没有 NSWindow，跳过激活策略检查。");
    }
    QVERIFY(before.alwaysOnTop);
    struct Restore {
        moyu::MainWindow* window;
        ~Restore() { moyu::MacWindow::setHidesFromDock(window, false, true); }
    } restore{&window};

    QVERIFY(moyu::MacWindow::setHidesFromDock(&window, true, true));
    QTest::qWait(30);
    QVERIFY2(window.isVisible(), "切到 Accessory 后窗口不应该被藏起来");
    const moyu::WindowChromeState after = moyu::MacWindow::chromeState(&window);
    QVERIFY2(after.hasNativeWindow, "切策略后 NSWindow 还在");
    QVERIFY2(after.alwaysOnTop, "切策略后置顶被重置了");
    QVERIFY2(after.joinsAllSpaces, "切策略后应该还在所有桌面");
}

void WindowTest::statusMenuHasEscapeActions()
{
    QWidget parent;
    const moyu::StatusMenu menu = moyu::createStatusMenu(&parent);
    QStringList titles;
    for (QAction* action : menu.menu->actions()) {
        if (!action->text().isEmpty()) {
            titles.push_back(action->text());
        }
    }
    QVERIFY(titles.contains(QStringLiteral("关闭穿透")));
    QVERIFY(titles.contains(QStringLiteral("窗口置顶")));
    QVERIFY(titles.contains(QStringLiteral("设置…")));
    QVERIFY(titles.contains(QStringLiteral("退出")));
    QVERIFY(menu.pin->isCheckable());
    QVERIFY(!menu.closeClickThrough->isEnabled());
}

void WindowTest::reservedHotkeysAndClickThroughGate()
{
    QVERIFY(moyu::isReservedSystemHotkey(Qt::Key_Q, Qt::ControlModifier));
    QVERIFY(moyu::isReservedSystemHotkey(Qt::Key_Tab, Qt::ControlModifier | Qt::ShiftModifier));
    QVERIFY(moyu::isReservedSystemHotkey(Qt::Key_Space, Qt::ControlModifier));
    QVERIFY(moyu::isReservedSystemHotkey(Qt::Key_H, Qt::ControlModifier));
    QVERIFY(moyu::isReservedSystemHotkey(Qt::Key_Escape, Qt::ControlModifier | Qt::AltModifier));
    QVERIFY(!moyu::isReservedSystemHotkey(Qt::Key_Q, Qt::AltModifier));
    QVERIFY(!moyu::isReservedSystemHotkey(Qt::Key_E, Qt::AltModifier));

    QCOMPARE(static_cast<int>(moyu::decideClickThrough(true, false, true)),
             static_cast<int>(moyu::ClickThroughDecision::StayOff));
    QCOMPARE(static_cast<int>(moyu::decideClickThrough(true, true, false)),
             static_cast<int>(moyu::ClickThroughDecision::StayOff));
    QCOMPARE(static_cast<int>(moyu::decideClickThrough(true, true, true)),
             static_cast<int>(moyu::ClickThroughDecision::TurnOn));
    QCOMPARE(static_cast<int>(moyu::decideClickThrough(false, false, false)),
             static_cast<int>(moyu::ClickThroughDecision::TurnOff));
    QCOMPARE(static_cast<int>(moyu::effectForHotkey(moyu::HotkeyRole::NextPage, true)),
             static_cast<int>(moyu::HotkeyEffect::Ignore));
    QCOMPARE(static_cast<int>(moyu::effectForHotkey(moyu::HotkeyRole::PreviousPage, true)),
             static_cast<int>(moyu::HotkeyEffect::Ignore));
    QCOMPARE(static_cast<int>(moyu::effectForHotkey(moyu::HotkeyRole::NextPage, false)),
             static_cast<int>(moyu::HotkeyEffect::PageNext));
    QCOMPARE(static_cast<int>(moyu::effectForHotkey(moyu::HotkeyRole::Boss, true)),
             static_cast<int>(moyu::HotkeyEffect::ToggleBoss));

    const moyu::HotkeyChord boss = moyu::defaultChord(moyu::HotkeyRole::Boss);
    const auto parsed = moyu::parseChord(moyu::serializeChord(boss));
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->keyCode, boss.keyCode);
    QCOMPARE(parsed->qtKey, static_cast<int>(Qt::Key_Q));
    QCOMPARE(parsed->qtModifiers, static_cast<int>(Qt::AltModifier));

    moyu::ReadingStyle paper = moyu::presetStyle(moyu::PagePreset::Paper, moyu::ReadingStyle{});
    const moyu::ReadingStyle loaded = moyu::readingStyleFromItems(moyu::readingStyleItems(paper), moyu::ReadingStyle{});
    QCOMPARE(static_cast<int>(loaded.preset), static_cast<int>(moyu::PagePreset::Paper));
    QCOMPARE(loaded.background, paper.background);
    QCOMPARE(loaded.text, paper.text);
}

void WindowTest::duplicateHotkeyRegistrationFails()
{
    moyu::HotkeyChord chord;
    chord.qtKey = Qt::Key_F19;
    chord.qtModifiers = Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier;
    chord.keyCode = 0x50;
    moyu::GlobalHotkeys first;
    if (!first.bind(moyu::HotkeyRole::Boss, chord)) {
        QSKIP("Carbon RegisterEventHotKey 在这个进程里不可用");
    }
    moyu::GlobalHotkeys second;
    QVERIFY2(!second.bind(moyu::HotkeyRole::NextPage, chord), "同一个组合被占用时应该注册失败");
}

void WindowTest::backStackReturnsToThePreviousPlace()
{
    const QString text = QStringLiteral("第1章 甲\n你好。\n");
    const QByteArray utf8 = text.toUtf8();
    moyu::ChapterIndex chapter;
    chapter.index = 0;
    chapter.title = QStringLiteral("第1章 甲");
    chapter.byteEnd = static_cast<quint64>(utf8.size());
    const moyu::ChapterContent content = moyu::makeChapterContent(chapter, utf8.constData(), static_cast<quint64>(utf8.size()));

    moyu::MainWindow window;
    window.showBook({chapter}, content, 0, static_cast<quint64>(utf8.size()), 0);
    window.prepareJump();
    window.prepareJump();

    int calls = 0;
    quint32 chapterIndex = 99;
    quint32 offset = 99;
    QObject::connect(&window, &moyu::MainWindow::anchorJumpRequested, &window, [&](quint32 jumpedChapter, quint32 jumpedOffset) {
        ++calls;
        chapterIndex = jumpedChapter;
        offset = jumpedOffset;
    });
    QVERIFY(window.goBack());
    QCOMPARE(calls, 1);
    QCOMPARE(chapterIndex, 0U);
    QCOMPARE(offset, 0U);
    QVERIFY(!window.goBack());
}

void WindowTest::bossHidePausesAutoScroll()
{
    const QString text = QStringLiteral("第1章 甲\n自动滚动的正文。\n");
    const QByteArray utf8 = text.toUtf8();
    moyu::ChapterIndex chapter;
    chapter.index = 0;
    chapter.title = QStringLiteral("第1章 甲");
    chapter.byteEnd = static_cast<quint64>(utf8.size());
    const moyu::ChapterContent content = moyu::makeChapterContent(chapter, utf8.constData(), static_cast<quint64>(utf8.size()));

    moyu::MainWindow window;
    window.resize(320, 220);
    window.showBook({chapter}, content, 0, static_cast<quint64>(utf8.size()), 0);
    window.show();
    QApplication::processEvents();
    window.setPageMode(true);
    QVERIFY(window.pageMode());
    for (QTimer* timer : window.findChildren<QTimer*>()) {
        // 打开书会跳到进度，进度指示用一次性定时器。翻页模式自己不能再开重复定时器。
        if (timer->isActive()) {
            QVERIFY2(timer->isSingleShot(), "翻页模式本身不应该启动定时器");
        }
    }
    window.setPageMode(false);

    QVERIFY(!window.autoScrollRunning());
    window.setAutoScroll(true);
    QVERIFY(window.autoScrollRunning());
    window.hideForBoss();
    QVERIFY(!window.autoScrollRunning());
    QVERIFY(window.autoScroll());
    window.showForBoss();
    QVERIFY(window.isVisible());
    QVERIFY(!window.isBossHidden());
    QVERIFY(window.autoScrollRunning());
    window.setAutoScroll(false);
    QVERIFY(!window.autoScrollRunning());
}

void WindowTest::searchShortcutOpensPanel()
{
    const QString text = QStringLiteral("第1章 甲\n可以搜索的正文。\n");
    const QByteArray utf8 = text.toUtf8();
    moyu::ChapterIndex chapter;
    chapter.index = 0;
    chapter.title = QStringLiteral("第1章 甲");
    chapter.byteEnd = static_cast<quint64>(utf8.size());
    const moyu::ChapterContent content = moyu::makeChapterContent(chapter, utf8.constData(), static_cast<quint64>(utf8.size()));

    moyu::MainWindow window;
    window.resize(420, 320);
    window.showBook({chapter}, content, 0, static_cast<quint64>(utf8.size()), 0);
    window.show();
    QTest::qWait(50);
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(200, 80));
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QWidget* target = window.focusWidget() != nullptr ? window.focusWidget() : static_cast<QWidget*>(&window);
    QTest::keyClick(target, Qt::Key_F, Qt::ControlModifier);
    QApplication::processEvents();

    auto* panel = window.findChild<moyu::SearchPanel*>();
    QVERIFY(panel != nullptr);
    QVERIFY(panel->isVisible());
}

void WindowTest::readerBackgroundMatchesResizeMargin()
{
    const QString text = QStringLiteral("第1章 甲\n边缘和正文应该是同一层背景。\n第二行仍然在左边距之外。\n");
    const QByteArray utf8 = text.toUtf8();
    moyu::ChapterIndex chapter;
    chapter.index = 0;
    chapter.title = QStringLiteral("第1章 甲");
    chapter.byteEnd = static_cast<quint64>(utf8.size());
    const moyu::ChapterContent content = moyu::makeChapterContent(chapter, utf8.constData(), static_cast<quint64>(utf8.size()));

    moyu::MainWindow window;
    window.resize(420, 320);
    window.showBook({chapter}, content, 0, static_cast<quint64>(utf8.size()), 0);
    window.show();
    QApplication::processEvents();

    QImage image(window.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    window.render(&image);

    const QColor edge = image.pixelColor(2, window.height() / 2);
    // 缩放边缘 6px，阅读区左边距 16px，这个点落在正文区里、文字左边。
    const QColor body = image.pixelColor(moyu::kResizeMarginPx + 4, 48);
    const QColor status = image.pixelColor(window.width() - moyu::kResizeMarginPx - 16, window.height() - moyu::kResizeMarginPx - 8);
    QCOMPARE(body, edge);
    QCOMPARE(status, edge);
    const QColor expected = moyu::styledBackground(moyu::ReadingStyle{}, false);
    QCOMPARE(edge.alpha(), expected.alpha());
    // ARGB32 预乘再还原，蓝色通道可能差 1。
    QVERIFY(qAbs(edge.red() - expected.red()) <= 1);
    QVERIFY(qAbs(edge.green() - expected.green()) <= 1);
    QVERIFY(qAbs(edge.blue() - expected.blue()) <= 1);
}

void WindowTest::shelfListsTitlePercentAndTime()
{
    moyu::ShelfDialog dialog;
    moyu::ShelfEntry entry;
    entry.id = 7;
    entry.title = QStringLiteral("甲");
    entry.percent = 12;
    entry.filePath = QStringLiteral("/no/such/book.txt");
    entry.lastReadAt = QDateTime(QDate(2026, 3, 4), QTime(5, 6)).toMSecsSinceEpoch();
    dialog.setEntries({entry});
    auto* table = dialog.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("甲（文件不在）"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("12%"));
    QCOMPARE(table->item(0, 2)->text(), QStringLiteral("2026-03-04 05:06"));
}

void WindowTest::localDropKeepsEveryPath()
{
    const QList<QUrl> urls = {QUrl::fromLocalFile(QStringLiteral("/a.txt")),
                              QUrl(QStringLiteral("https://example.com/b")),
                              QUrl::fromLocalFile(QStringLiteral("/c.epub"))};
    QCOMPARE(moyu::localFilePaths(urls), QStringList({QStringLiteral("/a.txt"), QStringLiteral("/c.epub")}));
}

void WindowTest::presetBackgroundsDiffer()
{
    const moyu::ReadingStyle dark = moyu::presetStyle(moyu::PagePreset::Dark, moyu::ReadingStyle{});
    const moyu::ReadingStyle white = moyu::presetStyle(moyu::PagePreset::White, moyu::ReadingStyle{});
    const moyu::ReadingStyle green = moyu::presetStyle(moyu::PagePreset::Green, moyu::ReadingStyle{});
    const moyu::ReadingStyle paper = moyu::presetStyle(moyu::PagePreset::Paper, moyu::ReadingStyle{});
    QVERIFY(dark.background != white.background);
    QVERIFY(green.background != paper.background);
    QVERIFY(white.background != paper.background);
    QCOMPARE(dark.background, QColor(28, 28, 30));
    QCOMPARE(white.background, QColor(255, 255, 255));
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    WindowTest tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_window.moc"
