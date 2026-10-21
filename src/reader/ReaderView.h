#pragma once

#include "core/Types.h"

#include <QAbstractScrollArea>
#include <QColor>
#include <QFont>

#include <functional>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

class QPainter;
class QTextLayout;
class QTimer;

namespace moyu {

enum class ReadMode { Scroll, Page };

// 自绘阅读区。滚动模式下把相邻章节拼进同一份文档，滚过边界时只在远端追加或去掉一章。
// 翻页模式从锚点逐行填满一页，不给整章分页。锚点仍然是「章节号 + 章内字节偏移」。
class ReaderView : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit ReaderView(QWidget* parent = nullptr);
    ~ReaderView() override;

    void setDocument(ChapterContent content, quint64 chapterByteStart, quint64 totalBytes);
    // 只接受紧挨着已加载章节的那一章。已经在文档里则什么都不做。
    bool attachChapter(ChapterContent content, quint64 chapterByteStart, quint64 totalBytes);
    void setChapterCount(quint32 count);
    void clearDocument();
    bool hasDocument() const { return m_hasDocument; }

    int anchorParagraph() const { return m_anchorParagraph; }
    void scrollToParagraph(int paragraph);
    void scrollToByteOffset(quint32 byteOffset);
    void pinToEnd();
    // 全局翻页键走这里。隐藏窗口时由 MainWindow 挡住，这里不再把窗口显示出来。
    void scrollPage(bool next);
    void setLayoutMetrics(const QFont& font, int lineSpacingPercent, int paragraphGap, int margin);
    void setColors(const QColor& page, const QColor& ink);
    void setReadMode(ReadMode mode);
    ReadMode readMode() const { return m_mode; }
    void setAutoScroll(bool on);
    void pauseAutoScroll(bool paused);
    bool autoScrollRunning() const;
    void setHighlight(quint32 chapter, quint32 byteOffset, quint32 length);
    void clearHighlight();
    bool hasHighlight() const { return m_highlight.active; }
    void setPaintProbe(std::function<void(qint64)> probe);

    quint32 byteOffsetInChapter();
    quint64 chapterByteStart() const;
    quint32 chapterIndex() const;
    QString chapterTitle() const;
    double progressRatio();
    quint32 firstLoadedChapter() const;
    quint32 lastLoadedChapter() const;
    int loadedChapterCount() const;
    int laidOutParagraphs() const;
    bool pageShowsChapter(quint32 chapter) const;
    QString anchorPreview(int codePoints) const;

Q_SIGNALS:
    void anchorChanged();
    void nextChapterRequested();
    void previousChapterRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool viewportEvent(QEvent* event) override;

private:
    struct LayoutEntry {
        int paragraph = -1;
        QString text;
        std::unique_ptr<QTextLayout> layout;
        std::vector<quint32> utf16ToByte;
        int height = 0;
    };
    struct Piece {
        ChapterContent content;
        quint64 chapterByteStart = 0;
        int rowBegin = 0;
        int rowEnd = 0;
    };
    struct RowRef {
        int piece = 0;
        int local = 0;
    };
    struct PageLine {
        int row = 0;
        int line = 0;
        int top = 0;
    };
    struct Highlight {
        bool active = false;
        quint32 chapter = 0;
        quint32 byte = 0;
        quint32 length = 0;
    };

    int textWidth() const;
    int lineHeight() const;
    int estimateHeight(const Paragraph& paragraph, int width) const;
    const moyu::Paragraph* paragraphForRow(int row) const;
    const Piece* pieceForRow(int row) const;
    void relayoutForWidth();
    void rebuildPrefix();
    void applyAnchorToScroll();
    void noteScroll();
    void clearLayouts();
    void shiftLayouts(int delta);
    LayoutEntry* layoutParagraph(int index);
    void ensureLayouts(int first, int last);
    int paragraphAt(int y) const;
    // 返回 true 表示这次滚动撞到了已加载内容的边缘。
    bool scrollBy(int delta);
    void noteEdge(bool forward);
    void trimWindow();
    void dropFront();
    void dropBack();
    void appendPiece(ChapterContent content, quint64 chapterByteStart, bool applyLeftover);
    void prependPiece(ChapterContent content, quint64 chapterByteStart);
    int pixelForAnchor();
    quint32 byteAtPixel(int paragraph, int pixel);
    void paintViewport(QPaintEvent* event);
    void paintHighlight(QPainter& painter, const LayoutEntry& entry, int row, int originY);
    void rebuildPage();
    void turnPage(bool next);
    bool stepLine(int& row, int& line, bool forward);
    quint32 byteForLine(const LayoutEntry& entry, int line) const;
    void updateAutoTimer();
    void placeAnchor(int paragraph, quint32 byte, bool notify);
    // 滚动、翻页、跳转时亮一下，停下约 1 秒后关掉。定时器只在这时创建。
    void revealProgressHint();
    void hideProgressHint();
    void paintProgressHint(QPainter& painter);

    std::vector<Piece> m_pieces;
    std::vector<RowRef> m_rows;
    quint64 m_totalBytes = 0;
    bool m_hasDocument = false;
    quint32 m_chapterCount = 0;
    bool m_hasChapterCount = false;
    quint32 m_focusChapter = 0;
    std::vector<int> m_heights;
    std::vector<int> m_prefix;
    int m_anchorParagraph = 0;
    quint32 m_anchorByte = 0;
    // 一次触控板手势（ScrollBegin 到 ScrollEnd）在边界上只预读一章。
    bool m_gestureTurnedChapter = false;
    quint32 m_edgeRequest = 0xFFFFFFFFu;
    int m_leftoverDelta = 0;
    bool m_applyingAnchor = false;
    bool m_stickToEnd = false;
    int m_contentWidth = -1;
    int m_margin = 16;
    int m_paragraphGap = 10;
    int m_lineSpacingPercent = 100;
    QColor m_pageColor{28, 28, 30, 210};
    QColor m_inkColor{255, 255, 255, 230};
    QFont m_font;
    ReadMode m_mode = ReadMode::Scroll;
    Highlight m_highlight;
    std::vector<PageLine> m_pageLines;
    bool m_pageHasNext = false;
    int m_pageNextRow = 0;
    quint32 m_pageNextByte = 0;
    int m_pageWheelAccum = 0;
    bool m_autoEnabled = false;
    bool m_autoPaused = false;
    QTimer* m_autoTimer = nullptr;
    bool m_hintVisible = false;
    QTimer* m_hintTimer = nullptr;
    std::function<void(qint64)> m_paintProbe;

    std::list<LayoutEntry> m_layouts;
    std::unordered_map<int, std::list<LayoutEntry>::iterator> m_layoutIndex;
};

}  // namespace moyu
