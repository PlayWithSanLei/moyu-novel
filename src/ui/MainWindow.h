#pragma once

#include "app/BookSession.h"
#include "core/Types.h"
#include "ui/ReadingStyle.h"
#include "ui/WindowGeometry.h"

#include <QStringList>
#include <QWidget>

#include <vector>

class QLabel;
class QMouseEvent;
class QShowEvent;
class QResizeEvent;
class QMoveEvent;
class QEvent;
class QTimer;
class QWindow;

namespace moyu {

class BookmarkDialog;
class ReaderView;
class SearchPanel;
class TocDrawer;

class MainWindow : public QWidget {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    void setAlwaysOnTop(bool on);
    bool alwaysOnTop() const { return m_alwaysOnTop; }

    void applyAppearance(const ReadingStyle& style);
    void setBackgroundOpacityPercent(int percent);
    void setClickThrough(bool on);
    bool clickThrough() const { return m_clickThrough; }
    void setHideOnBlur(bool on);
    void setSuspendBlurHide(bool on);
    bool isBossHidden() const { return m_bossHidden; }
    void hideForBoss();
    void showForBoss();
    // 窗口藏着时返回 false，并且不会把窗口显示出来。
    bool turnPageFromHotkey(bool next);

    void showToast(const QString& message);
    void showPreview(const ChapterContent& content, quint64 bodyByteStart, quint64 totalBytes);
    void showBook(const std::vector<ChapterIndex>& chapters,
                  const ChapterContent& content,
                  quint64 chapterByteStart,
                  quint64 totalBytes,
                  quint32 byteOffset);
    void showChapter(const ChapterContent& content,
                     quint64 chapterByteStart,
                     quint64 totalBytes,
                     quint32 byteOffset,
                     bool pinToEnd);
    void attachNeighbor(const ChapterContent& content, quint64 chapterByteStart, quint64 totalBytes);
    void clearBook();

    bool hasBook() const { return m_hasBook; }
    quint32 chapterIndex() const;
    quint32 firstLoadedChapter() const;
    quint32 lastLoadedChapter() const;
    int chapterCount() const;
    void setPageMode(bool on);
    bool pageMode() const { return m_pageMode; }
    void setAutoScroll(bool on);
    bool autoScroll() const { return m_autoScroll; }
    bool autoScrollRunning() const;
    void prepareJump();
    bool goBack();
    void showSearch();
    void showJump();
    void showBookmarks();
    void addBookmarkHere();
    void addSearchHits(const std::vector<SearchHitInfo>& hits, bool done);
    void setBookmarkList(const std::vector<Bookmark>& bookmarks);
    void armHighlight(quint32 chapter, quint32 offset, quint32 bytes);
    const ChapterContent* loadedChapter(quint32 index) const;
    quint32 anchorOffset() const;
    // 朗读到新的一段：不在屏幕上就翻过去；开了跟读标记就标出来。
    void followSpeech(quint32 chapter, quint32 byteStart, quint32 byteEnd);
    void setSpeechMarkStyle(bool on, const QColor& color);
    void clearSpeechMark();
    void setSpeechActive(bool on) { m_speechActive = on; }
    // 显示在状态栏末尾，空字符串表示不显示。
    void setSpeechStatus(const QString& text);

Q_SIGNALS:
    void openFileRequested(QString path);
    void openFilesRequested(QStringList paths);
    void chapterActivated(quint32 chapterIndex);
    void nextChapterRequested();
    void previousChapterRequested();
    void anchorMoved(quint32 chapterIndex, quint32 byteOffset);
    void anchorJumpRequested(quint32 chapter, quint32 offset);
    void searchRequested(QString text);
    void searchCancelRequested();
    void bookmarkAddRequested(quint32 chapter, quint32 offset, QString preview);
    void bookmarkDeleteRequested(qint64 id);
    void bookmarksRequested();
    void geometryChanged(QRect geometry);
    // 先藏窗口，再发这个信号。槽里去排队保存进度，不要在槽里同步等数据库。
    void bossHidden();
    void bossShown();
    void speechToggleRequested();
    void speechStopRequested();
    void speechStartAtRequested(quint32 chapter, quint32 byteOffset);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class DragMode { None, Move, Resize };

    void reapplyChrome();
    void watchNativeWindow();
    void beginManualDrag(DragMode mode, Qt::Edges edges, const QPoint& global);
    void updateHoverCursor(QPoint localPos);
    void layoutChildren();
    void updateStatus();
    void toggleToc();
    void showReadingSurface();
    void pushColors();

    bool m_alwaysOnTop = false;
    ReadingStyle m_style;
    bool m_clickThrough = false;
    bool m_hideOnBlur = false;
    bool m_suspendBlurHide = false;
    bool m_bossHidden = false;
    DragMode m_dragMode = DragMode::None;
    Qt::Edges m_dragEdges;
    QPoint m_dragOriginGlobal;
    QRect m_dragOriginGeometry;
    QWindow* m_watchedWindow = nullptr;

    ReaderView* m_reader = nullptr;
    TocDrawer* m_toc = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_toast = nullptr;
    QTimer* m_toastTimer = nullptr;
    bool m_hasBook = false;
    bool m_tocVisible = false;
    bool m_pageMode = false;
    bool m_autoScroll = false;
    std::vector<ChapterIndex> m_chapters;
    quint64 m_totalBytes = 0;
    quint32 m_highlightChapter = 0;
    quint32 m_highlightOffset = 0;
    quint32 m_highlightBytes = 0;
    struct Place {
        quint32 chapter = 0;
        quint32 offset = 0;
    };
    std::vector<Place> m_back;
    SearchPanel* m_search = nullptr;
    BookmarkDialog* m_bookmarks = nullptr;
    bool m_speechMarkOn = false;
    bool m_speechActive = false;
    QString m_speechStatus;
    QColor m_speechMarkColor;
};

}  // namespace moyu
