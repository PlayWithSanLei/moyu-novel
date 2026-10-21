#include "ui/MainWindow.h"
#include "ui/ShelfDialog.h"

#include "mac/MacWindow.h"
#include "reader/ReaderView.h"
#include "ui/ReadingTools.h"
#include "ui/Theme.h"
#include "ui/TocDrawer.h"

#include <QCursor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <limits>

namespace moyu {
namespace {

constexpr int kStatusHeight = 22;
constexpr int kTocWidth = 220;
constexpr int kToastMs = 2500;

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setWindowTitle(QStringLiteral("摸鱼阅读器"));

    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::transparent);
    setPalette(pal);

    // 子控件必须先于 resize 创建，第一帧布局才能把阅读区让出缩放边缘。
    m_reader = new ReaderView(this);
    m_toc = new TocDrawer(this);
    m_status = new QLabel(this);
    m_toast = new QLabel(this);
    m_toastTimer = new QTimer(this);
    m_reader->hide();
    m_toc->hide();
    m_status->hide();
    m_toast->hide();

    m_status->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_status->setAttribute(Qt::WA_TranslucentBackground, true);
    m_status->setAttribute(Qt::WA_NoSystemBackground, true);
    m_status->setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_status->setAutoFillBackground(false);
    m_status->setStyleSheet(QStringLiteral("color: rgba(255,255,255,200); background: transparent;"));
    m_toast->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_toast->setAlignment(Qt::AlignCenter);
    m_toast->setStyleSheet(QStringLiteral(
        "color: white; background-color: rgba(28,28,30,230); padding: 6px 12px;"));

    m_toastTimer->setSingleShot(true);
    connect(m_toastTimer, &QTimer::timeout, m_toast, &QWidget::hide);
    m_reader->viewport()->installEventFilter(this);

    connect(m_reader, &ReaderView::anchorChanged, this, [this] {
        updateStatus();
        if (m_hasBook) {
            m_toc->setCurrentChapter(m_reader->chapterIndex());
            Q_EMIT anchorMoved(m_reader->chapterIndex(), m_reader->byteOffsetInChapter());
        }
    });
    connect(m_reader, &ReaderView::nextChapterRequested, this, &MainWindow::nextChapterRequested);
    connect(m_reader, &ReaderView::previousChapterRequested, this, &MainWindow::previousChapterRequested);
    connect(m_toc, &TocDrawer::chapterActivated, this, [this](quint32 index) {
        prepareJump();
        m_highlightBytes = 0;
        m_reader->clearHighlight();
        Q_EMIT chapterActivated(index);
    });

    // macOS 上 Qt::CTRL 是 Command。菜单栏被藏起时，这些快捷键仍然挂在窗口上。
    auto* tocShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_T), this);
    connect(tocShortcut, &QShortcut::activated, this, [this] { toggleToc(); });
    auto* searchShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), this);
    connect(searchShortcut, &QShortcut::activated, this, [this] { showSearch(); });
    auto* backShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), this);
    connect(backShortcut, &QShortcut::activated, this, [this] { goBack(); });
    auto* bookmarkShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_D), this);
    connect(bookmarkShortcut, &QShortcut::activated, this, [this] { addBookmarkHere(); });
    auto* jumpShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_G), this);
    connect(jumpShortcut, &QShortcut::activated, this, [this] { showJump(); });
    // Command+W 只关窗口不退出，走老板键同一条路：停自动滚动、异步存进度。点 Dock 图标再打开。
    auto* closeShortcut = new QShortcut(QKeySequence::Close, this);
    connect(closeShortcut, &QShortcut::activated, this, [this] { hideForBoss(); });

    setMinimumSize(kMinWindowSize);
    resize(520, 360);
    layoutChildren();
    m_reader->setLayoutMetrics(m_style.makeFont(), m_style.lineSpacingPercent, m_style.paragraphGap, m_style.margin);
    pushColors();
}

void MainWindow::setAlwaysOnTop(bool on)
{
    m_alwaysOnTop = on;
    reapplyChrome();
}

void MainWindow::applyAppearance(const ReadingStyle& style)
{
    m_style = style;
    m_reader->setLayoutMetrics(style.makeFont(), style.lineSpacingPercent, style.paragraphGap, style.margin);
    pushColors();
}

void MainWindow::setBackgroundOpacityPercent(int percent)
{
    m_style.backgroundOpacityPercent = std::clamp(percent, 0, 100);
    pushColors();
}

void MainWindow::setClickThrough(bool on)
{
    m_clickThrough = on;
    MacWindow::setIgnoresMouseEvents(this, on);
    pushColors();
}

void MainWindow::setHideOnBlur(bool on)
{
    m_hideOnBlur = on;
}

void MainWindow::setSuspendBlurHide(bool on)
{
    m_suspendBlurHide = on;
}

void MainWindow::hideForBoss()
{
    if (m_bossHidden) {
        return;
    }
    // 先暂停自动滚动再藏窗口。信号里只排队保存进度，不要在这里等数据库。
    m_bossHidden = true;
    m_reader->pauseAutoScroll(true);
    if (m_search != nullptr) {
        m_search->hide();
    }
    if (m_bookmarks != nullptr) {
        m_bookmarks->hide();
    }
    MacWindow::hideWithoutAnimation(this);
    Q_EMIT bossHidden();
}

void MainWindow::showForBoss()
{
    m_bossHidden = false;
    MacWindow::showWithoutActivating(this);
    m_reader->pauseAutoScroll(false);
}

bool MainWindow::turnPageFromHotkey(bool next)
{
    if (m_bossHidden || !isVisible()) {
        return false;
    }
    if (m_hasBook) {
        m_reader->scrollPage(next);
    }
    return true;
}

void MainWindow::pushColors()
{
    const QColor page = styledBackground(m_style, m_clickThrough);
    const QColor ink = styledInk(m_style, m_clickThrough);
    m_reader->setColors(page, ink);
    m_toc->setColors(page, ink);
    m_status->setStyleSheet(QStringLiteral("color: rgba(%1,%2,%3,%4); background: transparent;")
                                .arg(ink.red())
                                .arg(ink.green())
                                .arg(ink.blue())
                                .arg(ink.alpha()));
    update();
}

void MainWindow::showToast(const QString& message)
{
    if (message.isEmpty()) {
        return;
    }
    m_toast->setText(message);
    m_toast->adjustSize();
    layoutChildren();
    m_toast->show();
    m_toast->raise();
    m_toastTimer->start(kToastMs);
}

void MainWindow::showPreview(const ChapterContent& content, quint64 bodyByteStart, quint64 totalBytes)
{
    m_toc->hide();
    showReadingSurface();
    m_reader->setDocument(content, bodyByteStart, totalBytes);
    updateStatus();
    m_reader->setFocus(Qt::OtherFocusReason);
}

void MainWindow::showBook(const std::vector<ChapterIndex>& chapters,
                          const ChapterContent& content,
                          quint64 chapterByteStart,
                          quint64 totalBytes,
                          quint32 byteOffset)
{
    m_chapters = chapters;
    m_totalBytes = totalBytes;
    m_back.clear();
    m_highlightBytes = 0;
    m_reader->clearHighlight();
    m_reader->setChapterCount(static_cast<quint32>(chapters.size()));
    m_toc->setChapters(chapters);
    showChapter(content, chapterByteStart, totalBytes, byteOffset, false);
}

void MainWindow::showChapter(const ChapterContent& content,
                             quint64 chapterByteStart,
                             quint64 totalBytes,
                             quint32 byteOffset,
                             bool pinToEnd)
{
    showReadingSurface();
    if (m_tocVisible) {
        m_toc->show();
    }
    m_totalBytes = totalBytes;
    m_reader->setDocument(content, chapterByteStart, totalBytes);
    if (pinToEnd) {
        m_reader->pinToEnd();
    } else {
        m_reader->scrollToByteOffset(byteOffset);
    }
    if (m_highlightBytes > 0 && content.index == m_highlightChapter) {
        m_reader->setHighlight(m_highlightChapter, m_highlightOffset, m_highlightBytes);
    } else {
        m_reader->clearHighlight();
    }
    m_toc->setCurrentChapter(content.index);
    updateStatus();
    m_reader->setFocus(Qt::OtherFocusReason);
}

void MainWindow::clearBook()
{
    m_hasBook = false;
    m_tocVisible = false;
    m_chapters.clear();
    m_totalBytes = 0;
    m_back.clear();
    m_highlightBytes = 0;
    m_reader->clearHighlight();
    m_reader->clearDocument();
    m_reader->hide();
    m_status->hide();
    m_toc->hide();
    if (m_search != nullptr) {
        m_search->hide();
    }
    if (m_bookmarks != nullptr) {
        m_bookmarks->hide();
    }
    setFocusProxy(nullptr);
    layoutChildren();
    update();
}

quint32 MainWindow::chapterIndex() const
{
    return m_reader->chapterIndex();
}

quint32 MainWindow::firstLoadedChapter() const
{
    return m_reader->firstLoadedChapter();
}

quint32 MainWindow::lastLoadedChapter() const
{
    return m_reader->lastLoadedChapter();
}

int MainWindow::chapterCount() const
{
    return static_cast<int>(m_chapters.size());
}

void MainWindow::attachNeighbor(const ChapterContent& content, quint64 chapterByteStart, quint64 totalBytes)
{
    if (!m_hasBook) {
        return;
    }
    m_reader->attachChapter(content, chapterByteStart, totalBytes);
}

void MainWindow::setPageMode(bool on)
{
    m_pageMode = on;
    m_reader->setReadMode(on ? ReadMode::Page : ReadMode::Scroll);
    updateStatus();
}

void MainWindow::setAutoScroll(bool on)
{
    m_autoScroll = on;
    m_reader->setAutoScroll(on);
}

bool MainWindow::autoScrollRunning() const
{
    return m_reader->autoScrollRunning();
}

void MainWindow::prepareJump()
{
    if (!m_hasBook) {
        return;
    }
    Place here;
    here.chapter = m_reader->chapterIndex();
    here.offset = m_reader->byteOffsetInChapter();
    if (!m_back.empty() && m_back.back().chapter == here.chapter && m_back.back().offset == here.offset) {
        return;
    }
    m_back.push_back(here);
    if (m_back.size() > 40) {
        m_back.erase(m_back.begin());
    }
}

bool MainWindow::goBack()
{
    if (!m_hasBook || m_back.empty()) {
        return false;
    }
    const Place place = m_back.back();
    m_back.pop_back();
    m_highlightBytes = 0;
    m_reader->clearHighlight();
    Q_EMIT anchorJumpRequested(place.chapter, place.offset);
    return true;
}

void MainWindow::showSearch()
{
    if (!m_hasBook || m_chapters.empty()) {
        return;
    }
    if (m_search == nullptr) {
        m_search = new SearchPanel(this);
        connect(m_search, &SearchPanel::searchRequested, this, [this](const QString& text) {
            // 换了关键字，上一次的高亮已经不对应结果列表了。
            m_highlightBytes = 0;
            m_reader->clearHighlight();
            Q_EMIT searchRequested(text);
        });
        connect(m_search, &SearchPanel::cancelRequested, this, &MainWindow::searchCancelRequested);
        connect(m_search, &SearchPanel::dismissed, this, [this] {
            m_highlightBytes = 0;
            m_reader->clearHighlight();
        });
        connect(m_search, &SearchPanel::hitChosen, this, [this](quint32 chapter, quint32 offset, quint32 bytes) {
            prepareJump();
            armHighlight(chapter, offset, bytes);
            Q_EMIT anchorJumpRequested(chapter, offset);
        });
    }
    m_search->show();
    m_search->raise();
    m_search->activateWindow();
}

void MainWindow::showJump()
{
    if (!m_hasBook || m_chapters.empty()) {
        return;
    }
    const int percentNow = std::clamp(static_cast<int>(m_reader->progressRatio() * 100.0 + 0.5), 0, 100);
    JumpDialog dialog(static_cast<int>(m_chapters.size()),
                      percentNow,
                      static_cast<int>(m_reader->chapterIndex()) + 1,
                      this);
    if (dialog.exec() != QDialog::Accepted || !dialog.acceptedJump()) {
        return;
    }
    prepareJump();
    m_highlightBytes = 0;
    m_reader->clearHighlight();
    if (dialog.byPercent()) {
        quint64 target = 0;
        if (m_totalBytes > 0) {
            target = (m_totalBytes * static_cast<quint64>(std::clamp(dialog.percent(), 0, 100))) / 100;
            if (target >= m_totalBytes) {
                target = m_totalBytes - 1;
            }
        }
        const ChapterIndex& last = m_chapters.back();
        quint32 chapter = last.index;
        quint32 offset = 0;
        for (const ChapterIndex& item : m_chapters) {
            if (target < item.byteEnd || item.index == last.index) {
                chapter = item.index;
                const quint64 local = target > item.byteStart ? target - item.byteStart : 0;
                offset = local > std::numeric_limits<quint32>::max() ? std::numeric_limits<quint32>::max()
                                                                      : static_cast<quint32>(local);
                break;
            }
        }
        Q_EMIT anchorJumpRequested(chapter, offset);
        return;
    }
    const int number = dialog.chapterNumber();
    if (number < 1 || number > static_cast<int>(m_chapters.size())) {
        return;
    }
    Q_EMIT chapterActivated(static_cast<quint32>(number - 1));
}

void MainWindow::showBookmarks()
{
    if (!m_hasBook || m_chapters.empty()) {
        return;
    }
    if (m_bookmarks == nullptr) {
        m_bookmarks = new BookmarkDialog(this);
        connect(m_bookmarks, &BookmarkDialog::openRequested, this, [this](quint32 chapter, quint32 offset) {
            prepareJump();
            m_highlightBytes = 0;
            m_reader->clearHighlight();
            Q_EMIT anchorJumpRequested(chapter, offset);
        });
        connect(m_bookmarks, &BookmarkDialog::deleteRequested, this, &MainWindow::bookmarkDeleteRequested);
    }
    m_bookmarks->show();
    m_bookmarks->raise();
    Q_EMIT bookmarksRequested();
}

void MainWindow::addBookmarkHere()
{
    if (!m_hasBook || m_chapters.empty()) {
        return;
    }
    Q_EMIT bookmarkAddRequested(m_reader->chapterIndex(), m_reader->byteOffsetInChapter(), m_reader->anchorPreview(50));
    showToast(QStringLiteral("已添加书签"));
}

void MainWindow::addSearchHits(const std::vector<SearchHitInfo>& hits, bool done)
{
    if (m_search == nullptr) {
        return;
    }
    if (!hits.empty()) {
        m_search->addHits(hits);
    }
    if (done) {
        m_search->markFinished();
    }
}

void MainWindow::setBookmarkList(const std::vector<Bookmark>& bookmarks)
{
    if (m_bookmarks != nullptr) {
        m_bookmarks->setBookmarks(bookmarks);
    }
}

void MainWindow::armHighlight(quint32 chapter, quint32 offset, quint32 bytes)
{
    m_highlightChapter = chapter;
    m_highlightOffset = offset;
    m_highlightBytes = bytes;
    m_reader->setHighlight(chapter, offset, bytes);
}

void MainWindow::showReadingSurface()
{
    m_hasBook = true;
    m_reader->show();
    m_status->show();
    setFocusProxy(m_reader);
    layoutChildren();
    update();
}

void MainWindow::toggleToc()
{
    if (!m_hasBook) {
        return;
    }
    m_tocVisible = !m_tocVisible;
    m_toc->setVisible(m_tocVisible);
    if (m_tocVisible) {
        m_toc->revealCurrent();
        m_reader->setFocus(Qt::OtherFocusReason);
    }
    layoutChildren();
}

void MainWindow::updateStatus()
{
    if (!m_hasBook) {
        return;
    }
    const double percent = m_reader->progressRatio() * 100.0;
    m_status->setText(QStringLiteral("%1    %2%    %3")
                          .arg(m_reader->chapterTitle())
                          .arg(percent, 0, 'f', 1)
                          .arg(m_pageMode ? QStringLiteral("翻页") : QStringLiteral("滚动")));
}

void MainWindow::layoutChildren()
{
    const QRect inner = rect().adjusted(kResizeMarginPx, kResizeMarginPx, -kResizeMarginPx, -kResizeMarginPx);
    const int statusHeight = m_status->isVisible() ? kStatusHeight : 0;
    int tocWidth = 0;
    if (m_tocVisible && m_toc->isVisible()) {
        tocWidth = std::min(kTocWidth, std::max(120, inner.width() / 3));
    }
    const int bodyHeight = std::max(0, inner.height() - statusHeight);
    m_toc->setGeometry(inner.left(), inner.top(), tocWidth, bodyHeight);
    m_reader->setGeometry(inner.left() + tocWidth, inner.top(), std::max(0, inner.width() - tocWidth), bodyHeight);
    m_status->setGeometry(inner.left(), inner.top() + bodyHeight, inner.width(), statusHeight);

    if (!m_toast->isVisible()) {
        return;
    }
    const QSize hint = m_toast->sizeHint();
    const int toastWidth = std::min(hint.width(), std::max(40, inner.width() - 24));
    const int toastHeight = std::max(hint.height(), 28);
    m_toast->setGeometry(inner.center().x() - toastWidth / 2,
                         inner.bottom() - toastHeight - 28,
                         toastWidth,
                         toastHeight);
}

void MainWindow::reapplyChrome()
{
    MacWindow::applyChrome(this, m_alwaysOnTop);
}

void MainWindow::watchNativeWindow()
{
    QWindow* handle = windowHandle();
    if (handle == nullptr || handle == m_watchedWindow) {
        return;
    }
    if (m_watchedWindow != nullptr) {
        disconnect(m_watchedWindow, nullptr, this, nullptr);
    }
    m_watchedWindow = handle;
    // show / hide 之后 Qt 可能把 level 和 collectionBehavior 写回默认值。
    connect(handle, &QWindow::visibleChanged, this, [this](bool visible) {
        if (visible) {
            reapplyChrome();
        }
    });
}

void MainWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_bossHidden = false;
    watchNativeWindow();
    reapplyChrome();
    layoutChildren();
    // QCocoaWindow::setVisible 有时在 show() 返回之后才重置 NSWindow。
    QTimer::singleShot(0, this, [this] { reapplyChrome(); });
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutChildren();
    Q_EMIT geometryChanged(geometry());
}

void MainWindow::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);
    Q_EMIT geometryChanged(geometry());
}

bool MainWindow::event(QEvent* event)
{
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::WindowDeactivate && m_hideOnBlur && !m_suspendBlurHide && !m_bossHidden && isVisible()) {
        hideForBoss();
    }
    if (event->type() == QEvent::WinIdChange && isVisible()) {
        if (m_watchedWindow != nullptr) {
            disconnect(m_watchedWindow, nullptr, this, nullptr);
            m_watchedWindow = nullptr;
        }
        watchNativeWindow();
        reapplyChrome();
    }
    return handled;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_reader->viewport() && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            // 正文上的按下用来拖窗口。滚动条不在 viewport 里，点它不会进这里。
            MacWindow::makeKey(this);
            m_reader->setFocus(Qt::MouseFocusReason);
            if (!MacWindow::trySystemMove(this)) {
                beginManualDrag(DragMode::Move, {}, mouse->globalPosition().toPoint());
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MainWindow::beginManualDrag(DragMode mode, Qt::Edges edges, const QPoint& global)
{
    m_dragMode = mode;
    m_dragEdges = edges;
    m_dragOriginGlobal = global;
    m_dragOriginGeometry = geometry();
    grabMouse();
    if (mode == DragMode::Resize) {
        setCursor(cursorForEdges(edges));
    }
}

void MainWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    MacWindow::makeKey(this);
    setFocus(Qt::MouseFocusReason);

    const Qt::Edges edges = edgesAt(event->position().toPoint(), size(), kResizeMarginPx);
    if (edges != Qt::Edges{}) {
        beginManualDrag(DragMode::Resize, edges, event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (MacWindow::trySystemMove(this)) {
        event->accept();
        return;
    }

    beginManualDrag(DragMode::Move, {}, event->globalPosition().toPoint());
    event->accept();
}

void MainWindow::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint delta = event->globalPosition().toPoint() - m_dragOriginGlobal;
    if (m_dragMode == DragMode::Resize) {
        setGeometry(applyEdgeResize(m_dragOriginGeometry, delta, m_dragEdges, kMinWindowSize));
        event->accept();
        return;
    }
    if (m_dragMode == DragMode::Move) {
        setGeometry(QRect(m_dragOriginGeometry.topLeft() + delta, m_dragOriginGeometry.size()));
        event->accept();
        return;
    }
    updateHoverCursor(event->position().toPoint());
    event->accept();
}

void MainWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragMode != DragMode::None && event->button() == Qt::LeftButton) {
        m_dragMode = DragMode::None;
        m_dragEdges = {};
        if (mouseGrabber() == this) {
            releaseMouse();
        }
        updateHoverCursor(event->position().toPoint());
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void MainWindow::leaveEvent(QEvent* event)
{
    if (m_dragMode == DragMode::None) {
        unsetCursor();
    }
    QWidget::leaveEvent(event);
}

void MainWindow::updateHoverCursor(QPoint localPos)
{
    setCursor(cursorForEdges(edgesAt(localPos, size(), kResizeMarginPx)));
}

void MainWindow::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    // 铺满整个矩形，包括四个角。圆角裁切会留下 alpha 为 0 的像素，点击会穿过去。
    painter.fillRect(rect(), styledBackground(m_style, m_clickThrough));
    if (m_hasBook) {
        return;
    }
    painter.setPen(styledInk(m_style, m_clickThrough));
    QFont titleFont = font();
    titleFont.setPointSize(16);
    painter.setFont(titleFont);
    painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("摸鱼阅读器"));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const QStringList paths = localFilePaths(event->mimeData()->urls());
    if (paths.isEmpty()) {
        return;
    }
    Q_EMIT openFilesRequested(paths);
    event->acceptProposedAction();
}

}  // namespace moyu
