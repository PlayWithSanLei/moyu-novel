#include "reader/ReaderView.h"

#include "app/StartupTiming.h"
#include "core/Utf8.h"

#include <QElapsedTimer>
#include <QHideEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTextLayout>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace moyu {
namespace {

constexpr int kBottomPad = 12;
constexpr int kLayoutCapacity = 200;
constexpr int kHintFadeMs = 1000;
constexpr int kHintWidth = 3;
constexpr quint32 kNoEdge = 0xFFFFFFFFu;

void keepViewportTransparent(QWidget* viewport)
{
    // 背景只由 MainWindow 画一层。视口再填一次，半透明会叠深，边缘缩放区就显得更浅。
    viewport->setAutoFillBackground(false);
    viewport->setAttribute(Qt::WA_NoSystemBackground, true);
    viewport->setAttribute(Qt::WA_TranslucentBackground, true);
    viewport->setAttribute(Qt::WA_OpaquePaintEvent, false);
    QPalette clear = viewport->palette();
    clear.setColor(QPalette::Window, Qt::transparent);
    clear.setColor(QPalette::Base, Qt::transparent);
    viewport->setPalette(clear);
};

}  // namespace

ReaderView::ReaderView(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 系统设置「显示滚动条：始终」时，AsNeeded 会画出不受背景透明度控制的实心槽。滚动条只记滚动值。
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    keepViewportTransparent(viewport());
    m_font = font();
    m_font.setPointSize(15);
    setFont(m_font);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { noteScroll(); });
}

ReaderView::~ReaderView() = default;

void ReaderView::setDocument(ChapterContent content, quint64 chapterByteStart, quint64 totalBytes)
{
    m_pieces.clear();
    m_rows.clear();
    m_heights.clear();
    m_prefix.clear();
    m_pageLines.clear();
    clearLayouts();
    m_totalBytes = totalBytes;
    m_hasDocument = true;
    m_focusChapter = content.index;
    m_anchorParagraph = 0;
    m_anchorByte = 0;
    m_stickToEnd = false;
    m_contentWidth = -1;
    m_edgeRequest = kNoEdge;
    m_leftoverDelta = 0;
    m_pageHasNext = false;
    hideProgressHint();
    appendPiece(std::move(content), chapterByteStart, false);
    m_contentWidth = -1;
    relayoutForWidth();
    updateAutoTimer();
}

bool ReaderView::attachChapter(ChapterContent content, quint64 chapterByteStart, quint64 totalBytes)
{
    if (totalBytes != 0) {
        m_totalBytes = totalBytes;
    }
    if (!m_hasDocument || m_pieces.empty()) {
        setDocument(std::move(content), chapterByteStart, m_totalBytes);
        return true;
    }
    const quint32 index = content.index;
    for (const Piece& piece : m_pieces) {
        if (piece.content.index == index) {
            if (m_edgeRequest == index) {
                m_edgeRequest = kNoEdge;
            }
            return false;
        }
    }
    const quint32 first = m_pieces.front().content.index;
    const quint32 last = m_pieces.back().content.index;
    bool placed = false;
    if (last < std::numeric_limits<quint32>::max() && index == last + 1) {
        appendPiece(std::move(content), chapterByteStart, true);
        placed = true;
    } else if (first > 0 && index + 1 == first) {
        prependPiece(std::move(content), chapterByteStart);
        placed = true;
    }
    if (!placed) {
        return false;
    }
    if (m_edgeRequest == index) {
        m_edgeRequest = kNoEdge;
    }
    trimWindow();
    if (m_mode == ReadMode::Page) {
        rebuildPage();
    }
    viewport()->update();
    return true;
}

void ReaderView::setChapterCount(quint32 count)
{
    m_chapterCount = count;
    m_hasChapterCount = true;
}

void ReaderView::clearDocument()
{
    m_pieces.clear();
    m_rows.clear();
    m_heights.clear();
    m_prefix.clear();
    m_pageLines.clear();
    m_totalBytes = 0;
    m_hasDocument = false;
    m_anchorParagraph = 0;
    m_anchorByte = 0;
    m_stickToEnd = false;
    m_edgeRequest = kNoEdge;
    m_leftoverDelta = 0;
    hideProgressHint();
    clearLayouts();
    rebuildPrefix();
    updateAutoTimer();
    viewport()->update();
}

void ReaderView::scrollPage(bool next)
{
    if (m_mode == ReadMode::Page) {
        turnPage(next);
        return;
    }
    const int line = lineHeight();
    const int page = std::max(line, viewport()->height() - line);
    scrollBy(next ? page : -page);
}

void ReaderView::setLayoutMetrics(const QFont& font, int lineSpacingPercent, int paragraphGap, int margin)
{
    const bool changed = font != m_font || lineSpacingPercent != m_lineSpacingPercent || paragraphGap != m_paragraphGap ||
                         margin != m_margin;
    m_font = font;
    m_lineSpacingPercent = std::clamp(lineSpacingPercent, 80, 200);
    m_paragraphGap = std::max(0, paragraphGap);
    m_margin = std::max(0, margin);
    setFont(m_font);
    if (changed && m_hasDocument) {
        m_contentWidth = -1;
        relayoutForWidth();
    } else {
        viewport()->update();
    }
}

void ReaderView::setColors(const QColor& page, const QColor& ink)
{
    if (m_pageColor == page && m_inkColor == ink) {
        return;
    }
    m_pageColor = page;
    m_inkColor = ink;
    viewport()->update();
}

void ReaderView::setReadMode(ReadMode mode)
{
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    if (mode == ReadMode::Page) {
        rebuildPage();
    } else {
        applyAnchorToScroll();
    }
    updateAutoTimer();
    viewport()->update();
}

void ReaderView::setAutoScroll(bool on)
{
    m_autoEnabled = on;
    updateAutoTimer();
}

void ReaderView::pauseAutoScroll(bool paused)
{
    m_autoPaused = paused;
    updateAutoTimer();
}

bool ReaderView::autoScrollRunning() const
{
    return m_autoTimer != nullptr && m_autoTimer->isActive();
}

void ReaderView::setHighlight(quint32 chapter, quint32 byteOffset, quint32 length)
{
    m_highlight.active = length > 0;
    m_highlight.chapter = chapter;
    m_highlight.byte = byteOffset;
    m_highlight.length = length;
    viewport()->update();
}

void ReaderView::clearHighlight()
{
    m_highlight.active = false;
    viewport()->update();
}

void ReaderView::setPaintProbe(std::function<void(qint64)> probe)
{
    m_paintProbe = std::move(probe);
}

void ReaderView::scrollToParagraph(int paragraph)
{
    if (!m_hasDocument || m_heights.empty()) {
        return;
    }
    const int last = static_cast<int>(m_heights.size()) - 1;
    const int row = std::clamp(paragraph, 0, last);
    const Paragraph* source = paragraphForRow(row);
    placeAnchor(row, source != nullptr ? source->byteStart : 0, true);
}

void ReaderView::scrollToByteOffset(quint32 byteOffset)
{
    if (!m_hasDocument || m_rows.empty()) {
        return;
    }
    int paragraph = -1;
    const int count = static_cast<int>(m_rows.size());
    for (int i = 0; i < count; ++i) {
        if (m_pieces[static_cast<std::size_t>(m_rows[static_cast<std::size_t>(i)].piece)].content.index != m_focusChapter) {
            continue;
        }
        const Paragraph* source = paragraphForRow(i);
        if (source != nullptr && source->byteStart <= byteOffset) {
            paragraph = i;
        }
    }
    if (paragraph < 0) {
        paragraph = 0;
    }
    LayoutEntry* entry = layoutParagraph(paragraph);
    if (entry != nullptr && entry->layout != nullptr && !entry->utf16ToByte.empty()) {
        int utf16 = 0;
        const int mapSize = static_cast<int>(entry->utf16ToByte.size());
        for (int i = 0; i < mapSize; ++i) {
            if (entry->utf16ToByte[static_cast<std::size_t>(i)] >= byteOffset) {
                utf16 = i;
                break;
            }
            utf16 = i;
        }
        const QTextLine line = entry->layout->lineForTextPosition(utf16);
        if (line.isValid()) {
            const int textStart = line.textStart();
            if (textStart >= 0 && textStart < mapSize) {
                byteOffset = entry->utf16ToByte[static_cast<std::size_t>(textStart)];
            }
        }
        if (m_heights[static_cast<std::size_t>(paragraph)] != entry->height) {
            m_heights[static_cast<std::size_t>(paragraph)] = entry->height;
            rebuildPrefix();
        }
    }
    placeAnchor(paragraph, byteOffset, true);
}

void ReaderView::pinToEnd()
{
    if (!m_hasDocument || m_heights.empty()) {
        return;
    }
    if (m_mode == ReadMode::Page) {
        const int last = static_cast<int>(m_rows.size()) - 1;
        const Paragraph* source = paragraphForRow(last);
        m_anchorParagraph = last;
        m_anchorByte = source != nullptr ? source->byteEnd : 0;
        turnPage(false);
        return;
    }
    m_stickToEnd = true;
    const int last = static_cast<int>(m_heights.size()) - 1;
    if (LayoutEntry* entry = layoutParagraph(last)) {
        m_heights[static_cast<std::size_t>(last)] = entry->height;
        rebuildPrefix();
    }
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    noteScroll();
}

quint32 ReaderView::byteOffsetInChapter()
{
    if (!m_hasDocument || m_rows.empty()) {
        return 0;
    }
    return m_anchorByte;
}

quint64 ReaderView::chapterByteStart() const
{
    const Piece* piece = pieceForRow(m_anchorParagraph);
    return piece != nullptr ? piece->chapterByteStart : 0;
}

quint32 ReaderView::chapterIndex() const
{
    const Piece* piece = pieceForRow(m_anchorParagraph);
    return piece != nullptr ? piece->content.index : 0;
}

QString ReaderView::chapterTitle() const
{
    const Piece* piece = pieceForRow(m_anchorParagraph);
    return piece != nullptr ? piece->content.title : QString();
}

double ReaderView::progressRatio()
{
    if (!m_hasDocument || m_totalBytes == 0) {
        return 0;
    }
    const quint64 position = chapterByteStart() + byteOffsetInChapter();
    const quint64 clamped = std::min(position, m_totalBytes);
    return static_cast<double>(clamped) / static_cast<double>(m_totalBytes);
}

quint32 ReaderView::firstLoadedChapter() const
{
    return m_pieces.empty() ? 0 : m_pieces.front().content.index;
}

quint32 ReaderView::lastLoadedChapter() const
{
    return m_pieces.empty() ? 0 : m_pieces.back().content.index;
}

int ReaderView::loadedChapterCount() const
{
    return static_cast<int>(m_pieces.size());
}

int ReaderView::laidOutParagraphs() const
{
    return static_cast<int>(m_layoutIndex.size());
}

bool ReaderView::pageShowsChapter(quint32 chapter) const
{
    for (const PageLine& line : m_pageLines) {
        const Piece* piece = pieceForRow(line.row);
        if (piece != nullptr && piece->content.index == chapter) {
            return true;
        }
    }
    return false;
}

QString ReaderView::anchorPreview(int codePoints) const
{
    const Piece* piece = pieceForRow(m_anchorParagraph);
    if (piece == nullptr || codePoints <= 0) {
        return {};
    }
    const QByteArray& utf8 = piece->content.utf8;
    quint64 cursor = std::min<quint64>(m_anchorByte, static_cast<quint64>(utf8.size()));
    QString text;
    int count = 0;
    while (cursor < static_cast<quint64>(utf8.size()) && count < codePoints) {
        const int consumed = utf8SequenceLength(utf8.constData() + cursor, utf8.constData() + utf8.size());
        if (consumed <= 0) {
            break;
        }
        text += QString::fromUtf8(utf8.constData() + cursor, consumed);
        cursor += static_cast<quint64>(consumed);
        ++count;
    }
    return text;
}

void ReaderView::paintEvent(QPaintEvent* /*event*/)
{
    // 不填色。视口盖住这块区域，进度条画在 paintViewport 里才看得见。
}

void ReaderView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    if (!m_hasDocument) {
        rebuildPrefix();
        return;
    }
    if (textWidth() != m_contentWidth) {
        relayoutForWidth();
    } else if (m_mode == ReadMode::Page) {
        rebuildPage();
        viewport()->update();
    } else {
        rebuildPrefix();
        if (m_stickToEnd) {
            verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        } else {
            applyAnchorToScroll();
        }
        viewport()->update();
    }
}

void ReaderView::showEvent(QShowEvent* event)
{
    QAbstractScrollArea::showEvent(event);
    keepViewportTransparent(viewport());
    updateAutoTimer();
}

void ReaderView::hideEvent(QHideEvent* event)
{
    QAbstractScrollArea::hideEvent(event);
    updateAutoTimer();
}

void ReaderView::keyPressEvent(QKeyEvent* event)
{
    const int line = lineHeight();
    const int page = std::max(line, viewport()->height() - line);
    int delta = 0;
    switch (event->key()) {
    case Qt::Key_Up:
        delta = -line;
        break;
    case Qt::Key_Down:
        delta = line;
        break;
    case Qt::Key_Left:
    case Qt::Key_PageUp:
        delta = -page;
        break;
    case Qt::Key_Right:
    case Qt::Key_PageDown:
    case Qt::Key_Space:
        delta = page;
        break;
    default:
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    if (m_mode == ReadMode::Page && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        turnPage(delta > 0);
    } else {
        scrollBy(delta);
    }
    event->accept();
}

void ReaderView::wheelEvent(QWheelEvent* event)
{
    const Qt::ScrollPhase phase = event->phase();
    if (phase == Qt::ScrollBegin || phase == Qt::ScrollEnd) {
        m_gestureTurnedChapter = false;
        m_pageWheelAccum = 0;
    }
    if (m_mode == ReadMode::Page) {
        int steps = 0;
        if (!event->pixelDelta().isNull()) {
            m_pageWheelAccum += -event->pixelDelta().y();
            const int threshold = std::max(24, lineHeight() * 2);
            if (m_pageWheelAccum >= threshold) {
                steps = 1;
                m_pageWheelAccum = 0;
            } else if (m_pageWheelAccum <= -threshold) {
                steps = -1;
                m_pageWheelAccum = 0;
            }
        } else if (event->angleDelta().y() != 0) {
            steps = event->angleDelta().y() > 0 ? -1 : 1;
        }
        if (steps != 0) {
            turnPage(steps > 0);
        }
        event->accept();
        return;
    }
    int delta = 0;
    if (!event->pixelDelta().isNull()) {
        delta = -event->pixelDelta().y();
    } else if (event->angleDelta().y() != 0) {
        const int steps = event->angleDelta().y() / 120;
        delta = -steps * lineHeight() * 3;
        if (delta == 0) {
            delta = event->angleDelta().y() < 0 ? lineHeight() : -lineHeight();
        }
    }
    if (delta != 0) {
        // 同一次手势里，边界上只预读一章。没有相位的鼠标滚轮每次仍然算一次。
        if (phase == Qt::ScrollUpdate && m_gestureTurnedChapter) {
            event->accept();
            return;
        }
        const bool turned = scrollBy(delta);
        if (turned && phase == Qt::ScrollUpdate) {
            m_gestureTurnedChapter = true;
        }
    }
    event->accept();
}

bool ReaderView::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::Paint) {
        paintViewport(static_cast<QPaintEvent*>(event));
        return true;
    }
    return QAbstractScrollArea::viewportEvent(event);
}

int ReaderView::textWidth() const
{
    return std::max(1, viewport()->width() - m_margin * 2);
}

int ReaderView::lineHeight() const
{
    const int base = std::max(1, QFontMetrics(m_font).lineSpacing());
    return std::max(1, base * m_lineSpacingPercent / 100);
}

int ReaderView::estimateHeight(const Paragraph& paragraph, int width) const
{
    const int line = lineHeight();
    if (paragraph.codePoints <= 0 || width <= 0) {
        return line;
    }
    const int charWidth = std::max(1, QFontMetrics(m_font).horizontalAdvance(QStringLiteral("中")));
    const int perLine = std::max(1, width / charWidth);
    const int lines = std::max(1, (paragraph.codePoints + perLine - 1) / perLine);
    return lines * line;
}

const Paragraph* ReaderView::paragraphForRow(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return nullptr;
    }
    const RowRef& ref = m_rows[static_cast<std::size_t>(row)];
    if (ref.piece < 0 || ref.piece >= static_cast<int>(m_pieces.size())) {
        return nullptr;
    }
    const std::vector<Paragraph>& paragraphs = m_pieces[static_cast<std::size_t>(ref.piece)].content.paragraphs;
    if (ref.local < 0 || ref.local >= static_cast<int>(paragraphs.size())) {
        return nullptr;
    }
    return &paragraphs[static_cast<std::size_t>(ref.local)];
}

const ReaderView::Piece* ReaderView::pieceForRow(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return nullptr;
    }
    const int piece = m_rows[static_cast<std::size_t>(row)].piece;
    if (piece < 0 || piece >= static_cast<int>(m_pieces.size())) {
        return nullptr;
    }
    return &m_pieces[static_cast<std::size_t>(piece)];
}

void ReaderView::relayoutForWidth()
{
    m_contentWidth = textWidth();
    clearLayouts();
    const int count = static_cast<int>(m_rows.size());
    m_heights.assign(static_cast<std::size_t>(count), lineHeight());
    for (int i = 0; i < count; ++i) {
        const Paragraph* source = paragraphForRow(i);
        m_heights[static_cast<std::size_t>(i)] = source != nullptr ? estimateHeight(*source, m_contentWidth) : lineHeight();
    }
    rebuildPrefix();
    if (m_mode == ReadMode::Page) {
        rebuildPage();
    } else if (m_stickToEnd) {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    } else {
        applyAnchorToScroll();
    }
    viewport()->update();
}

void ReaderView::rebuildPrefix()
{
    const int count = static_cast<int>(m_heights.size());
    m_prefix.assign(static_cast<std::size_t>(count) + 1, 0);
    int y = 0;
    for (int i = 0; i < count; ++i) {
        m_prefix[static_cast<std::size_t>(i)] = y;
        y += m_heights[static_cast<std::size_t>(i)];
        if (i + 1 < count) {
            y += m_paragraphGap;
        }
    }
    if (count > 0) {
        y += kBottomPad;
    }
    m_prefix[static_cast<std::size_t>(count)] = y;

    auto* bar = verticalScrollBar();
    QSignalBlocker blocker(bar);
    const int viewportHeight = std::max(1, viewport()->height());
    const int maximum = std::max(0, y - viewportHeight);
    bar->setRange(0, maximum);
    bar->setPageStep(viewportHeight);
    bar->setSingleStep(lineHeight());
}

int ReaderView::pixelForAnchor()
{
    const int last = static_cast<int>(m_heights.size()) - 1;
    const int paragraph = std::clamp(m_anchorParagraph, 0, std::max(0, last));
    LayoutEntry* entry = layoutParagraph(paragraph);
    if (entry == nullptr || entry->layout == nullptr || entry->utf16ToByte.empty()) {
        return 0;
    }
    int utf16 = 0;
    const int mapSize = static_cast<int>(entry->utf16ToByte.size());
    for (int i = 0; i < mapSize; ++i) {
        if (entry->utf16ToByte[static_cast<std::size_t>(i)] >= m_anchorByte) {
            utf16 = i;
            break;
        }
        utf16 = i;
    }
    const QTextLine line = entry->layout->lineForTextPosition(utf16);
    if (!line.isValid()) {
        return 0;
    }
    return std::max(0, static_cast<int>(std::floor(line.y())));
}

quint32 ReaderView::byteAtPixel(int paragraph, int pixel)
{
    const Paragraph* source = paragraphForRow(paragraph);
    if (source == nullptr) {
        return m_anchorByte;
    }
    LayoutEntry* entry = layoutParagraph(paragraph);
    if (entry == nullptr || entry->layout == nullptr || entry->utf16ToByte.empty()) {
        return source->byteStart;
    }
    int utf16 = 0;
    const int lines = entry->layout->lineCount();
    for (int i = 0; i < lines; ++i) {
        const QTextLine line = entry->layout->lineAt(i);
        if (!line.isValid()) {
            break;
        }
        const int top = static_cast<int>(std::floor(line.y()));
        const int bottom = top + std::max(1, static_cast<int>(std::ceil(line.height())));
        if (pixel < bottom || i + 1 == lines) {
            utf16 = line.textStart();
            break;
        }
    }
    if (utf16 < 0) {
        utf16 = 0;
    }
    if (utf16 >= static_cast<int>(entry->utf16ToByte.size())) {
        utf16 = static_cast<int>(entry->utf16ToByte.size()) - 1;
    }
    return entry->utf16ToByte[static_cast<std::size_t>(utf16)];
}

void ReaderView::applyAnchorToScroll()
{
    if (m_mode == ReadMode::Page || m_heights.empty() || m_prefix.empty()) {
        return;
    }
    const int last = static_cast<int>(m_heights.size()) - 1;
    m_anchorParagraph = std::clamp(m_anchorParagraph, 0, last);
    const int pixel = pixelForAnchor();
    const int maximum = verticalScrollBar()->maximum();
    int value = m_prefix[static_cast<std::size_t>(m_anchorParagraph)] + pixel;
    value = std::clamp(value, 0, maximum);
    m_applyingAnchor = true;
    verticalScrollBar()->setValue(value);
    m_applyingAnchor = false;
}

void ReaderView::placeAnchor(int paragraph, quint32 byte, bool notify)
{
    m_anchorParagraph = paragraph;
    m_anchorByte = byte;
    m_stickToEnd = false;
    if (m_mode == ReadMode::Page) {
        rebuildPage();
        viewport()->update();
    } else {
        applyAnchorToScroll();
        trimWindow();
    }
    if (notify) {
        revealProgressHint();
        Q_EMIT anchorChanged();
    }
}

void ReaderView::noteScroll()
{
    if (m_mode == ReadMode::Page || m_applyingAnchor || m_prefix.size() < 2 || m_heights.empty()) {
        return;
    }
    revealProgressHint();
    const int value = verticalScrollBar()->value();
    if (value < verticalScrollBar()->maximum()) {
        m_stickToEnd = false;
    }
    const auto it = std::upper_bound(m_prefix.begin(), m_prefix.end(), value);
    int paragraph = static_cast<int>(std::distance(m_prefix.begin(), it)) - 1;
    const int last = static_cast<int>(m_heights.size()) - 1;
    paragraph = std::clamp(paragraph, 0, last);
    int pixel = value - m_prefix[static_cast<std::size_t>(paragraph)];
    const int limit = std::max(0, m_heights[static_cast<std::size_t>(paragraph)] - 1);
    pixel = std::clamp(pixel, 0, limit);
    const quint32 byte = byteAtPixel(paragraph, pixel);
    const bool changed = paragraph != m_anchorParagraph || byte != m_anchorByte;
    m_anchorParagraph = paragraph;
    m_anchorByte = byte;
    trimWindow();
    if (changed) {
        Q_EMIT anchorChanged();
    }
}

void ReaderView::clearLayouts()
{
    m_layouts.clear();
    m_layoutIndex.clear();
}

void ReaderView::shiftLayouts(int delta)
{
    m_layoutIndex.clear();
    for (auto it = m_layouts.begin(); it != m_layouts.end();) {
        it->paragraph += delta;
        if (it->paragraph < 0 || it->paragraph >= static_cast<int>(m_rows.size())) {
            it = m_layouts.erase(it);
            continue;
        }
        m_layoutIndex[it->paragraph] = it;
        ++it;
    }
}

ReaderView::LayoutEntry* ReaderView::layoutParagraph(int index)
{
    const Paragraph* source = paragraphForRow(index);
    const Piece* piece = pieceForRow(index);
    if (source == nullptr || piece == nullptr) {
        return nullptr;
    }
    const auto found = m_layoutIndex.find(index);
    if (found != m_layoutIndex.end()) {
        m_layouts.splice(m_layouts.begin(), m_layouts, found->second);
        return &(*found->second);
    }

    const quint32 length = source->byteEnd - source->byteStart;
    if (static_cast<quint64>(source->byteStart) + length > static_cast<quint64>(piece->content.utf8.size())) {
        return nullptr;
    }
    DecodedText decoded =
        decodeParagraph(piece->content.utf8.constData() + source->byteStart, length, source->byteStart);

    LayoutEntry entry;
    entry.paragraph = index;
    entry.text = std::move(decoded.text);
    entry.utf16ToByte = std::move(decoded.utf16ToByte);
    m_layouts.push_front(std::move(entry));
    LayoutEntry& stored = m_layouts.front();
    stored.layout = std::make_unique<QTextLayout>(stored.text);
    QFont layoutFont = m_font;
    if (source->kind == ParagraphKind::Heading) {
        layoutFont.setBold(true);
    }
    stored.layout->setFont(layoutFont);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    stored.layout->setTextOption(option);
    stored.layout->beginLayout();
    qreal y = 0;
    const qreal width = m_contentWidth > 0 ? m_contentWidth : textWidth();
    while (true) {
        QTextLine line = stored.layout->createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(width);
        line.setPosition(QPointF(0, y));
        const qreal step = line.height() * static_cast<qreal>(m_lineSpacingPercent) / 100.0;
        y += step;
    }
    stored.layout->endLayout();
    const int measured = static_cast<int>(std::ceil(y));
    stored.height = std::max(lineHeight(), measured);

    m_layoutIndex[index] = m_layouts.begin();
    while (static_cast<int>(m_layouts.size()) > kLayoutCapacity) {
        const int old = m_layouts.back().paragraph;
        m_layoutIndex.erase(old);
        m_layouts.pop_back();
    }
    return &m_layouts.front();
}

void ReaderView::ensureLayouts(int first, int last)
{
    if (m_heights.empty()) {
        return;
    }
    bool changed = false;
    const int count = static_cast<int>(m_heights.size());
    first = std::clamp(first, 0, count - 1);
    last = std::clamp(last, 0, count - 1);
    for (int i = first; i <= last; ++i) {
        LayoutEntry* entry = layoutParagraph(i);
        if (entry == nullptr) {
            continue;
        }
        if (m_heights[static_cast<std::size_t>(i)] != entry->height) {
            m_heights[static_cast<std::size_t>(i)] = entry->height;
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    rebuildPrefix();
    if (m_mode == ReadMode::Page) {
        return;
    }
    if (m_stickToEnd) {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    } else {
        applyAnchorToScroll();
    }
}

int ReaderView::paragraphAt(int y) const
{
    if (m_prefix.size() < 2) {
        return 0;
    }
    const auto it = std::upper_bound(m_prefix.begin(), m_prefix.end(), y);
    int paragraph = static_cast<int>(std::distance(m_prefix.begin(), it)) - 1;
    const int last = static_cast<int>(m_heights.size()) - 1;
    return std::clamp(paragraph, 0, std::max(0, last));
}

bool ReaderView::scrollBy(int delta)
{
    if (!m_hasDocument) {
        return false;
    }
    if (m_mode == ReadMode::Page) {
        if (delta != 0) {
            turnPage(delta > 0);
        }
        return false;
    }
    auto* bar = verticalScrollBar();
    const int value = bar->value();
    const int room = delta > 0 ? bar->maximum() - value : value - bar->minimum();
    const int applied = delta > 0 ? std::min(delta, std::max(0, room)) : std::max(delta, -std::max(0, room));
    if (applied != 0) {
        bar->setValue(value + applied);
    }
    const bool hitEnd = delta > 0 && value + applied >= bar->maximum();
    const bool hitStart = delta < 0 && value + applied <= bar->minimum();
    if (hitEnd) {
        m_leftoverDelta = delta - applied;
        noteEdge(true);
        return true;
    }
    if (hitStart) {
        m_leftoverDelta = 0;
        noteEdge(false);
        return true;
    }
    m_edgeRequest = kNoEdge;
    m_leftoverDelta = 0;
    return false;
}

void ReaderView::noteEdge(bool forward)
{
    if (m_pieces.empty()) {
        return;
    }
    if (forward) {
        const quint32 last = m_pieces.back().content.index;
        if (last == std::numeric_limits<quint32>::max()) {
            return;
        }
        const quint32 want = last + 1;
        if (m_hasChapterCount && want >= m_chapterCount) {
            return;
        }
        if (m_edgeRequest == want) {
            return;
        }
        m_edgeRequest = want;
        Q_EMIT nextChapterRequested();
        return;
    }
    const quint32 first = m_pieces.front().content.index;
    if (first == 0) {
        return;
    }
    const quint32 want = first - 1;
    if (m_edgeRequest == want) {
        return;
    }
    m_edgeRequest = want;
    Q_EMIT previousChapterRequested();
}

void ReaderView::trimWindow()
{
    if (m_pieces.size() < 2 || m_prefix.size() < 2 || m_mode == ReadMode::Page) {
        return;
    }
    const int top = verticalScrollBar()->value();
    const int bottom = top + std::max(1, viewport()->height());
    int firstVisible = m_rows[static_cast<std::size_t>(paragraphAt(top))].piece;
    int lastVisible = m_rows[static_cast<std::size_t>(paragraphAt(std::max(top, bottom - 1)))].piece;
    int keepFrom = std::max(0, firstVisible - 1);
    int keepTo = std::min(static_cast<int>(m_pieces.size()) - 1, lastVisible + 1);
    while (keepFrom > 0 && m_pieces.size() > 1) {
        dropFront();
        --keepFrom;
        --keepTo;
    }
    while (static_cast<int>(m_pieces.size()) - 1 > keepTo && m_pieces.size() > 1) {
        dropBack();
    }
}

void ReaderView::dropFront()
{
    if (m_pieces.size() < 2) {
        return;
    }
    const int cut = m_pieces.front().rowEnd;
    if (cut <= 0 || cut >= static_cast<int>(m_rows.size())) {
        return;
    }
    const int delta = m_prefix.size() > static_cast<std::size_t>(cut) ? m_prefix[static_cast<std::size_t>(cut)] : 0;
    const int oldScroll = verticalScrollBar()->value();
    m_pieces.erase(m_pieces.begin());
    m_rows.erase(m_rows.begin(), m_rows.begin() + cut);
    m_heights.erase(m_heights.begin(), m_heights.begin() + cut);
    for (RowRef& row : m_rows) {
        --row.piece;
    }
    for (Piece& piece : m_pieces) {
        piece.rowBegin -= cut;
        piece.rowEnd -= cut;
    }
    m_anchorParagraph = std::max(0, m_anchorParagraph - cut);
    shiftLayouts(-cut);
    rebuildPrefix();
    m_applyingAnchor = true;
    verticalScrollBar()->setValue(std::max(0, oldScroll - delta));
    m_applyingAnchor = false;
}

void ReaderView::dropBack()
{
    if (m_pieces.size() < 2) {
        return;
    }
    const int cut = m_pieces.back().rowBegin;
    if (cut <= 0 || cut > static_cast<int>(m_rows.size())) {
        return;
    }
    const int oldScroll = verticalScrollBar()->value();
    m_pieces.pop_back();
    m_rows.erase(m_rows.begin() + cut, m_rows.end());
    m_heights.erase(m_heights.begin() + cut, m_heights.end());
    if (m_anchorParagraph >= static_cast<int>(m_rows.size())) {
        m_anchorParagraph = std::max(0, static_cast<int>(m_rows.size()) - 1);
    }
    shiftLayouts(0);
    rebuildPrefix();
    m_applyingAnchor = true;
    verticalScrollBar()->setValue(std::min(oldScroll, verticalScrollBar()->maximum()));
    m_applyingAnchor = false;
}

void ReaderView::appendPiece(ChapterContent content, quint64 chapterByteStart, bool applyLeftover)
{
    const int oldScroll = verticalScrollBar()->value();
    Piece piece;
    piece.content = std::move(content);
    piece.chapterByteStart = chapterByteStart;
    piece.rowBegin = static_cast<int>(m_rows.size());
    const int localCount = static_cast<int>(piece.content.paragraphs.size());
    const int pieceIndex = static_cast<int>(m_pieces.size());
    const int width = textWidth();
    for (int i = 0; i < localCount; ++i) {
        m_rows.push_back(RowRef{pieceIndex, i});
        m_heights.push_back(estimateHeight(piece.content.paragraphs[static_cast<std::size_t>(i)], width));
    }
    piece.rowEnd = static_cast<int>(m_rows.size());
    m_pieces.push_back(std::move(piece));
    rebuildPrefix();
    if (m_mode == ReadMode::Page) {
        return;
    }
    m_applyingAnchor = true;
    verticalScrollBar()->setValue(std::min(oldScroll, verticalScrollBar()->maximum()));
    m_applyingAnchor = false;
    if (!applyLeftover || m_leftoverDelta <= 0) {
        return;
    }
    const int extra = std::min(m_leftoverDelta, std::max(1, viewport()->height()));
    m_leftoverDelta = 0;
    scrollBy(extra);
}

void ReaderView::prependPiece(ChapterContent content, quint64 chapterByteStart)
{
    const int oldScroll = verticalScrollBar()->value();
    Piece piece;
    piece.content = std::move(content);
    piece.chapterByteStart = chapterByteStart;
    piece.rowBegin = 0;
    const int localCount = static_cast<int>(piece.content.paragraphs.size());
    const int width = textWidth();
    std::vector<RowRef> rows;
    std::vector<int> heights;
    rows.reserve(static_cast<std::size_t>(localCount));
    heights.reserve(static_cast<std::size_t>(localCount));
    for (int i = 0; i < localCount; ++i) {
        rows.push_back(RowRef{0, i});
        heights.push_back(estimateHeight(piece.content.paragraphs[static_cast<std::size_t>(i)], width));
    }
    for (RowRef& row : m_rows) {
        ++row.piece;
    }
    for (Piece& existing : m_pieces) {
        existing.rowBegin += localCount;
        existing.rowEnd += localCount;
    }
    piece.rowEnd = localCount;
    m_rows.insert(m_rows.begin(), rows.begin(), rows.end());
    m_heights.insert(m_heights.begin(), heights.begin(), heights.end());
    m_pieces.insert(m_pieces.begin(), std::move(piece));
    m_anchorParagraph += localCount;
    shiftLayouts(localCount);
    rebuildPrefix();
    if (m_mode == ReadMode::Page) {
        return;
    }
    const int added = m_prefix.size() > static_cast<std::size_t>(localCount) ? m_prefix[static_cast<std::size_t>(localCount)] : 0;
    m_applyingAnchor = true;
    verticalScrollBar()->setValue(std::min(verticalScrollBar()->maximum(), std::max(0, oldScroll + added)));
    m_applyingAnchor = false;
}

quint32 ReaderView::byteForLine(const LayoutEntry& entry, int line) const
{
    if (entry.layout == nullptr || entry.utf16ToByte.empty()) {
        return 0;
    }
    const QTextLine textLine = entry.layout->lineAt(line);
    if (!textLine.isValid()) {
        return entry.utf16ToByte.front();
    }
    int utf16 = textLine.textStart();
    if (utf16 < 0) {
        utf16 = 0;
    }
    if (utf16 >= static_cast<int>(entry.utf16ToByte.size())) {
        utf16 = static_cast<int>(entry.utf16ToByte.size()) - 1;
    }
    return entry.utf16ToByte[static_cast<std::size_t>(utf16)];
}

bool ReaderView::stepLine(int& row, int& line, bool forward)
{
    LayoutEntry* entry = layoutParagraph(row);
    if (entry == nullptr || entry->layout == nullptr) {
        return false;
    }
    if (forward) {
        if (line + 1 < entry->layout->lineCount()) {
            ++line;
            return true;
        }
        if (row + 1 >= static_cast<int>(m_rows.size())) {
            return false;
        }
        ++row;
        line = 0;
        return layoutParagraph(row) != nullptr;
    }
    if (line > 0) {
        --line;
        return true;
    }
    if (row <= 0) {
        return false;
    }
    --row;
    LayoutEntry* previous = layoutParagraph(row);
    if (previous == nullptr || previous->layout == nullptr || previous->layout->lineCount() <= 0) {
        return false;
    }
    line = previous->layout->lineCount() - 1;
    return true;
}

void ReaderView::rebuildPage()
{
    m_pageLines.clear();
    m_pageHasNext = false;
    m_pageNextRow = m_anchorParagraph;
    m_pageNextByte = m_anchorByte;
    if (m_rows.empty()) {
        return;
    }
    const int viewHeight = std::max(1, viewport()->height());
    int row = std::clamp(m_anchorParagraph, 0, static_cast<int>(m_rows.size()) - 1);
    int y = 0;
    bool started = false;
    for (; row < static_cast<int>(m_rows.size()); ++row) {
        LayoutEntry* entry = layoutParagraph(row);
        if (entry == nullptr || entry->layout == nullptr) {
            continue;
        }
        const int lines = entry->layout->lineCount();
        for (int index = 0; index < lines; ++index) {
            const QTextLine line = entry->layout->lineAt(index);
            if (!line.isValid()) {
                continue;
            }
            const quint32 byte = byteForLine(*entry, index);
            if (!started) {
                quint32 nextByte = std::numeric_limits<quint32>::max();
                if (index + 1 < lines) {
                    nextByte = byteForLine(*entry, index + 1);
                }
                if (nextByte <= m_anchorByte) {
                    continue;
                }
                started = true;
            }
            const int height = std::max(1, static_cast<int>(std::ceil(line.height() * m_lineSpacingPercent / 100.0)));
            if (y > 0 && y + height > viewHeight) {
                m_pageHasNext = true;
                m_pageNextRow = row;
                m_pageNextByte = byte;
                return;
            }
            PageLine painted;
            painted.row = row;
            painted.line = index;
            painted.top = y;
            m_pageLines.push_back(painted);
            y += height;
        }
    }
    const quint32 last = m_pieces.empty() ? 0 : m_pieces.back().content.index;
    if (m_hasChapterCount && last < std::numeric_limits<quint32>::max() && last + 1 < m_chapterCount) {
        m_pageHasNext = true;
        m_pageNextRow = -1;
    }
}

void ReaderView::turnPage(bool next)
{
    if (!m_hasDocument || m_rows.empty()) {
        return;
    }
    if (m_pageLines.empty()) {
        rebuildPage();
    }
    if (next) {
        if (!m_pageHasNext) {
            noteEdge(true);
            revealProgressHint();
            return;
        }
        if (m_pageNextRow < 0) {
            noteEdge(true);
            revealProgressHint();
            return;
        }
        placeAnchor(m_pageNextRow, m_pageNextByte, true);
        return;
    }
    const int viewHeight = std::max(1, viewport()->height());
    int row = std::clamp(m_anchorParagraph, 0, static_cast<int>(m_rows.size()) - 1);
    int line = 0;
    if (LayoutEntry* entry = layoutParagraph(row)) {
        if (entry->layout != nullptr) {
            const int lines = entry->layout->lineCount();
            for (int index = 0; index < lines; ++index) {
                const quint32 byte = byteForLine(*entry, index);
                quint32 nextByte = std::numeric_limits<quint32>::max();
                if (index + 1 < lines) {
                    nextByte = byteForLine(*entry, index + 1);
                }
                if (byte <= m_anchorByte && m_anchorByte < nextByte) {
                    line = index;
                    break;
                }
                line = index;
            }
        }
    }
    int accumulated = 0;
    int targetRow = row;
    int targetLine = line;
    bool moved = false;
    while (accumulated < viewHeight) {
        if (!stepLine(row, line, false)) {
            break;
        }
        LayoutEntry* entry = layoutParagraph(row);
        int height = lineHeight();
        if (entry != nullptr && entry->layout != nullptr) {
            const QTextLine textLine = entry->layout->lineAt(line);
            if (textLine.isValid()) {
                height = std::max(1, static_cast<int>(std::ceil(textLine.height() * m_lineSpacingPercent / 100.0)));
            }
        }
        accumulated += height;
        targetRow = row;
        targetLine = line;
        moved = true;
    }
    if (!moved) {
        noteEdge(false);
        revealProgressHint();
        return;
    }
    LayoutEntry* entry = layoutParagraph(targetRow);
    const quint32 byte = entry != nullptr ? byteForLine(*entry, targetLine) : 0;
    placeAnchor(targetRow, byte, true);
}

void ReaderView::revealProgressHint()
{
    if (!m_hasDocument) {
        return;
    }
    m_hintVisible = true;
    if (m_hintTimer == nullptr) {
        // 一次性定时器，停下约 1 秒后淡出。空闲时不创建，避免常驻定时器。
        m_hintTimer = new QTimer(this);
        m_hintTimer->setSingleShot(true);
        m_hintTimer->setInterval(kHintFadeMs);
        connect(m_hintTimer, &QTimer::timeout, this, [this] {
            m_hintVisible = false;
            viewport()->update();
        });
    }
    m_hintTimer->start();
    viewport()->update();
}

void ReaderView::hideProgressHint()
{
    m_hintVisible = false;
    if (m_hintTimer != nullptr) {
        m_hintTimer->stop();
    }
}

void ReaderView::paintProgressHint(QPainter& painter)
{
    if (!m_hintVisible || !m_hasDocument) {
        return;
    }
    const int viewW = viewport()->width();
    const int viewH = viewport()->height();
    if (viewW < kHintWidth + 2 || viewH < 8) {
        return;
    }
    int thumb = 28;
    if (m_mode == ReadMode::Scroll) {
        const int extent = verticalScrollBar()->maximum() + verticalScrollBar()->pageStep();
        if (extent > 0) {
            thumb = viewH * verticalScrollBar()->pageStep() / extent;
        }
    }
    thumb = std::clamp(thumb, std::min(16, viewH), viewH);
    const double ratio = std::clamp(progressRatio(), 0.0, 1.0);
    const int travel = std::max(0, viewH - thumb);
    const int y = static_cast<int>(std::lround(ratio * static_cast<double>(travel)));
    const qreal x = static_cast<qreal>(viewW - 2 - kHintWidth);
    painter.save();
    // 空矩形配上 NoClip 在 Qt 6 里会把裁剪收成空，进度条就画不出来。
    painter.setClipping(false);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    // m_inkColor 已经带上文字透明度和移出变淡。
    painter.setBrush(m_inkColor);
    painter.drawRoundedRect(QRectF(x, y, kHintWidth, thumb), kHintWidth / 2.0, kHintWidth / 2.0);
    painter.restore();
}

void ReaderView::updateAutoTimer()
{
    const bool run = m_autoEnabled && !m_autoPaused && m_hasDocument && isVisible();
    if (!run) {
        if (m_autoTimer != nullptr) {
            m_autoTimer->stop();
        }
        return;
    }
    if (m_autoTimer == nullptr) {
        m_autoTimer = new QTimer(this);
        connect(m_autoTimer, &QTimer::timeout, this, [this] {
            if (m_mode == ReadMode::Page) {
                turnPage(true);
            } else {
                scrollBy(1);
            }
        });
    }
    m_autoTimer->setInterval(m_mode == ReadMode::Page ? 4000 : 33);
    if (!m_autoTimer->isActive()) {
        m_autoTimer->start();
    }
}

void ReaderView::paintHighlight(QPainter& painter, const LayoutEntry& entry, int row, int originY)
{
    if (!m_highlight.active || entry.layout == nullptr) {
        return;
    }
    const Piece* piece = pieceForRow(row);
    if (piece == nullptr || piece->content.index != m_highlight.chapter) {
        return;
    }
    const quint32 begin = m_highlight.byte;
    const quint32 end = begin + m_highlight.length;
    int utf16Begin = -1;
    int utf16End = -1;
    const int mapSize = std::max(0, static_cast<int>(entry.utf16ToByte.size()) - 1);
    for (int i = 0; i < mapSize; ++i) {
        const quint32 byte = entry.utf16ToByte[static_cast<std::size_t>(i)];
        if (utf16Begin < 0 && byte >= begin) {
            utf16Begin = i;
        }
        if (byte < end) {
            utf16End = i + 1;
        }
    }
    if (utf16Begin < 0 || utf16End <= utf16Begin) {
        return;
    }
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 196, 60, 140));
    const int lines = entry.layout->lineCount();
    for (int index = 0; index < lines; ++index) {
        const QTextLine line = entry.layout->lineAt(index);
        if (!line.isValid()) {
            continue;
        }
        const int from = std::max(utf16Begin, line.textStart());
        const int to = std::min(utf16End, line.textStart() + line.textLength());
        if (from >= to) {
            continue;
        }
        const qreal x1 = line.cursorToX(from);
        const qreal x2 = line.cursorToX(to);
        const qreal top = originY + line.y();
        painter.drawRect(QRectF(m_margin + std::min(x1, x2), top, std::abs(x2 - x1), std::max<qreal>(1, line.height())));
    }
    painter.restore();
}

void ReaderView::paintViewport(QPaintEvent* event)
{
    QElapsedTimer timer;
    if (m_paintProbe) {
        timer.start();
    }
    QPainter painter(viewport());
    painter.setClipRect(event->rect());
    if (!m_hasDocument) {
        if (m_paintProbe) {
            m_paintProbe(timer.nsecsElapsed());
        }
        return;
    }
    painter.setPen(m_inkColor);
    painter.setFont(m_font);

    if (m_mode == ReadMode::Page) {
        if (m_pageLines.empty()) {
            rebuildPage();
        }
        noteTextPainted();
        for (const PageLine& painted : m_pageLines) {
            LayoutEntry* entry = layoutParagraph(painted.row);
            if (entry == nullptr || entry->layout == nullptr) {
                continue;
            }
            const QTextLine line = entry->layout->lineAt(painted.line);
            if (!line.isValid()) {
                continue;
            }
            paintHighlight(painter, *entry, painted.row, painted.top - static_cast<int>(std::floor(line.y())));
            line.draw(&painter, QPointF(m_margin, painted.top - line.y()));
        }
        paintProgressHint(painter);
        if (m_paintProbe) {
            m_paintProbe(timer.nsecsElapsed());
        }
        return;
    }

    if (!m_heights.empty()) {
        const int count = static_cast<int>(m_heights.size());
        int origin = verticalScrollBar()->value();
        int first = std::max(0, paragraphAt(origin) - 1);
        int last = std::min(count - 1, paragraphAt(origin + viewport()->height()) + 1);
        ensureLayouts(first, last);
        int guard = 0;
        while (last + 1 < count && guard < count && static_cast<int>(m_prefix.size()) > last) {
            origin = verticalScrollBar()->value();
            const int endY = m_prefix[static_cast<std::size_t>(last)] + m_heights[static_cast<std::size_t>(last)];
            if (endY >= origin + viewport()->height()) {
                break;
            }
            ++last;
            ensureLayouts(last, last);
            ++guard;
        }
        origin = verticalScrollBar()->value();
        first = std::max(0, paragraphAt(origin) - 1);
        noteTextPainted();
        for (int i = first; i <= last && i < count; ++i) {
            const int y = m_prefix[static_cast<std::size_t>(i)] - origin;
            if (y > viewport()->height() || y + m_heights[static_cast<std::size_t>(i)] < 0) {
                continue;
            }
            LayoutEntry* entry = layoutParagraph(i);
            if (entry != nullptr && entry->layout != nullptr) {
                paintHighlight(painter, *entry, i, y);
                entry->layout->draw(&painter, QPointF(m_margin, y));
            }
        }
    }
    paintProgressHint(painter);
    if (m_paintProbe) {
        m_paintProbe(timer.nsecsElapsed());
    }
}

}  // namespace moyu
