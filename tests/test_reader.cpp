#include "app/BookSession.h"
#include "core/ChapterDetector.h"
#include "core/Database.h"
#include "core/MappedFile.h"
#include "reader/ReaderView.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <optional>
#include <vector>

class ReaderTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void anchorParagraphSurvivesWiderResize();
    void englishTextFillsViewportAfterEstimate();
    void scrollMomentumDoesNotSkipChapters();
    void wheelBurstRequestsOneChapter();
    void anchorByteSurvivesWiderResize();
    void updatedFileKeepsClampedProgress();
    void prependKeepsByteAndMovesScroll();
    void appendKeepsScrollAndByte();
    void scrollingIntoLaterChapterDropsTheFarOne();
    void momentumCrossesLoadedChaptersWithoutRequest();
    void pageModeLayoutsOnlyTheVisiblePage();
    void pageTurnSpansChapterBoundary();
    void autoScrollRunsOnlyWhileEnabled();
    void highlightPaintsTheKeyword();
    void preloadedNeighborIsAttached();
    void bookmarkAddListAndDelete();
    void paintOfHundredMegabyteBook();
    void scrollBarStaysHiddenAndHintTimerIsOneShot();
};

namespace {

moyu::ChapterContent makeChapter(quint32 index, const QString& title, const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    moyu::ChapterIndex chapter;
    chapter.index = index;
    chapter.title = title;
    chapter.byteStart = 0;
    chapter.byteEnd = static_cast<quint64>(utf8.size());
    return moyu::makeChapterContent(chapter, utf8.constData(), static_cast<quint64>(utf8.size()));
}

QString tallBody(int lines, const QString& label)
{
    QString body;
    for (int i = 0; i < lines; ++i) {
        body += QStringLiteral("%1的第%2段，用来把这一章撑高。\n").arg(label).arg(i);
    }
    return body;
}

}  // namespace

void ReaderTest::anchorParagraphSurvivesWiderResize()
{
    QString text;
    for (int i = 0; i < 40; ++i) {
        text += QStringLiteral("段落内容%1，窗口变宽之后锚点应该还在这一段。\n").arg(i);
    }
    const QByteArray utf8 = text.toUtf8();
    moyu::ChapterIndex chapter;
    chapter.title = QStringLiteral("第一章");
    chapter.byteEnd = static_cast<quint64>(utf8.size());
    moyu::ChapterContent content = moyu::makeChapterContent(chapter, utf8.constData(), static_cast<quint64>(utf8.size()));
    QVERIFY(content.paragraphs.size() > 10U);

    moyu::ReaderView view;
    view.resize(300, 220);
    view.show();
    view.setDocument(std::move(content), 0, static_cast<quint64>(utf8.size()));
    view.scrollToParagraph(10);
    QCOMPARE(view.anchorParagraph(), 10);

    view.resize(560, 220);
    QCOMPARE(view.anchorParagraph(), 10);
}

void ReaderTest::englishTextFillsViewportAfterEstimate()
{
    QString text;
    for (int i = 0; i < 40; ++i) {
        text += QStringLiteral("hello world this is a narrow english paragraph\n");
    }
    const QByteArray utf8 = text.toUtf8();
    moyu::ChapterIndex chapter;
    chapter.title = QStringLiteral("Chapter 1");
    chapter.byteEnd = static_cast<quint64>(utf8.size());
    moyu::ChapterContent content = moyu::makeChapterContent(chapter, utf8.constData(), static_cast<quint64>(utf8.size()));
    QVERIFY(content.paragraphs.size() >= 40U);

    moyu::ReaderView view;
    view.resize(420, 460);
    view.show();
    view.setDocument(std::move(content), 0, static_cast<quint64>(utf8.size()));
    QVERIFY(view.verticalScrollBar()->maximum() > 0);
    view.viewport()->repaint();

    const QImage image = view.viewport()->grab().toImage();
    QVERIFY(!image.isNull());
    int lowestInk = -1;
    for (int y = image.height() - 1; y >= 0; --y) {
        bool ink = false;
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > 180 && pixel.green() > 180 && pixel.blue() > 180) {
                ink = true;
                break;
            }
        }
        if (ink) {
            lowestInk = y;
            break;
        }
    }
    QVERIFY2(lowestInk >= image.height() - 48,
             qPrintable(QStringLiteral("最下方有字的行是 %1，图高 %2").arg(lowestInk).arg(image.height())));
}

void ReaderTest::scrollMomentumDoesNotSkipChapters()
{
    const QByteArray utf8 = QStringLiteral("短\n").toUtf8();
    moyu::ChapterIndex chapter;
    chapter.byteEnd = static_cast<quint64>(utf8.size());
    moyu::ChapterContent content = moyu::makeChapterContent(chapter, utf8.constData(), static_cast<quint64>(utf8.size()));

    moyu::ReaderView view;
    view.resize(300, 400);
    view.show();
    view.setDocument(std::move(content), 0, static_cast<quint64>(utf8.size()));
    QCOMPARE(view.verticalScrollBar()->maximum(), 0);

    int jumps = 0;
    QObject::connect(&view, &moyu::ReaderView::nextChapterRequested, &view, [&jumps] { ++jumps; });
    for (int i = 0; i < 5; ++i) {
        QWheelEvent event(QPointF(20, 20),
                          QPointF(20, 20),
                          QPoint(0, -30),
                          QPoint(0, 0),
                          Qt::NoButton,
                          Qt::NoModifier,
                          Qt::ScrollMomentum,
                          false);
        QApplication::sendEvent(view.viewport(), &event);
    }
    // 惯性可以往下预读，但同一段惯性只发一次，也不会把还没拼进来的章节直接换上来。
    QCOMPARE(jumps, 1);
    QCOMPARE(view.chapterIndex(), 0U);
}

void ReaderTest::wheelBurstRequestsOneChapter()
{
    const QByteArray utf8 = QStringLiteral("短\n").toUtf8();
    moyu::ChapterIndex chapter;
    chapter.byteEnd = static_cast<quint64>(utf8.size());
    moyu::ChapterContent content = moyu::makeChapterContent(chapter, utf8.constData(), static_cast<quint64>(utf8.size()));

    moyu::ReaderView view;
    view.resize(300, 400);
    view.show();
    view.setDocument(std::move(content), 0, static_cast<quint64>(utf8.size()));
    QCOMPARE(view.verticalScrollBar()->maximum(), 0);

    int jumps = 0;
    QObject::connect(&view, &moyu::ReaderView::nextChapterRequested, &view, [&jumps] { ++jumps; });
    QWheelEvent begin(QPointF(20, 20),
                      QPointF(20, 20),
                      QPoint(0, 0),
                      QPoint(0, 0),
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::ScrollBegin,
                      false);
    QApplication::sendEvent(view.viewport(), &begin);
    for (int i = 0; i < 5; ++i) {
        QWheelEvent event(QPointF(20, 20),
                          QPointF(20, 20),
                          QPoint(0, -24),
                          QPoint(0, 0),
                          Qt::NoButton,
                          Qt::NoModifier,
                          Qt::ScrollUpdate,
                          false);
        QApplication::sendEvent(view.viewport(), &event);
    }
    QCOMPARE(jumps, 1);
}

void ReaderTest::anchorByteSurvivesWiderResize()
{
    QString text(1200, QLatin1Char('a'));
    text += QLatin1Char('\n');
    const QByteArray utf8 = text.toUtf8();
    moyu::ChapterIndex chapter;
    chapter.byteEnd = static_cast<quint64>(utf8.size());
    moyu::ChapterContent content = moyu::makeChapterContent(chapter, utf8.constData(), static_cast<quint64>(utf8.size()));

    moyu::ReaderView view;
    view.resize(280, 180);
    view.show();
    view.setDocument(std::move(content), 0, static_cast<quint64>(utf8.size()));
    view.scrollToByteOffset(500);
    const quint32 before = view.byteOffsetInChapter();
    QVERIFY(before > 0);

    view.resize(720, 180);
    QCOMPARE(view.byteOffsetInChapter(), before);
}

void ReaderTest::updatedFileKeepsClampedProgress()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    moyu::Database database(dir.filePath(QStringLiteral("moyu.db")));
    QVERIFY(database.startAndWait());
    moyu::BookSession session(&database, dir.path());

    const QString path = dir.filePath(QStringLiteral("serial.txt"));
    QString first;
    for (int i = 1; i <= 6; ++i) {
        first += QStringLiteral("第%1章 旧\n正文一段，足够留下进度。\n").arg(i);
    }
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(first.toUtf8()) > 0);
    }

    std::optional<moyu::TxtDocument> opened;
    QObject::connect(&session, &moyu::BookSession::bookReady, &session, [&opened](const moyu::TxtDocument& document) {
        opened = document;
    });
    session.openFile(path);
    QTRY_VERIFY(opened.has_value());
    QCOMPARE(opened->parsed.chapters.size(), 6U);
    session.noteAnchor(5, 40);
    session.flushNow();

    const QString second = QStringLiteral("第1章 新\n啊\n第2章 新\n啊\n第3章 新\n啊\n");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(second.toUtf8()) == second.toUtf8().size());
    }
    opened.reset();
    session.openFile(path);
    QTRY_VERIFY(opened.has_value());
    QCOMPARE(opened->parsed.chapters.size(), 3U);
    QCOMPARE(opened->chapterIndex, 2U);
    const auto length = opened->parsed.chapters[2].byteEnd - opened->parsed.chapters[2].byteStart;
    QVERIFY(opened->byteOffset <= length);

    auto found = database.findByFingerprint(opened->parsed.book.fingerprint);
    QVERIFY(found);
    QVERIFY(found->has_value());
    QCOMPARE((*found)->book.filePath, path);
}

void ReaderTest::prependKeepsByteAndMovesScroll()
{
    const QString body = tallBody(60, QStringLiteral("当前章"));
    const quint64 currentBytes = static_cast<quint64>(body.toUtf8().size());
    moyu::ReaderView view;
    view.resize(320, 200);
    view.show();
    view.setDocument(makeChapter(1, QStringLiteral("第二章"), body), 1000, currentBytes + 1000);
    view.scrollToByteOffset(200);
    const int scroll = view.verticalScrollBar()->value();
    const quint32 byte = view.byteOffsetInChapter();
    QVERIFY(scroll > 0);
    QCOMPARE(view.chapterIndex(), 1U);

    QVERIFY(view.attachChapter(makeChapter(0, QStringLiteral("第一章"), tallBody(40, QStringLiteral("上一章"))), 0, currentBytes + 1000));
    QCOMPARE(view.chapterIndex(), 1U);
    QCOMPARE(view.byteOffsetInChapter(), byte);
    QVERIFY(view.verticalScrollBar()->value() > scroll);
    QCOMPARE(view.firstLoadedChapter(), 0U);
}

void ReaderTest::appendKeepsScrollAndByte()
{
    const QString body = tallBody(60, QStringLiteral("本章"));
    const quint64 bytes = static_cast<quint64>(body.toUtf8().size());
    moyu::ReaderView view;
    view.resize(320, 200);
    view.show();
    view.setDocument(makeChapter(0, QStringLiteral("第一章"), body), 0, bytes * 2);
    view.scrollToByteOffset(80);
    const int scroll = view.verticalScrollBar()->value();
    const quint32 byte = view.byteOffsetInChapter();
    QVERIFY(scroll > 0);
    QVERIFY(scroll < view.verticalScrollBar()->maximum());

    QVERIFY(view.attachChapter(makeChapter(1, QStringLiteral("第二章"), tallBody(40, QStringLiteral("下一章"))), bytes, bytes * 2));
    QCOMPARE(view.verticalScrollBar()->value(), scroll);
    QCOMPARE(view.byteOffsetInChapter(), byte);
    QCOMPARE(view.chapterIndex(), 0U);
    QCOMPARE(view.lastLoadedChapter(), 1U);
}

void ReaderTest::scrollingIntoLaterChapterDropsTheFarOne()
{
    const QString body = tallBody(80, QStringLiteral("章"));
    const quint64 bytes = static_cast<quint64>(body.toUtf8().size());
    moyu::ReaderView view;
    view.resize(300, 180);
    view.show();
    view.setChapterCount(3);
    view.setDocument(makeChapter(0, QStringLiteral("第一章"), body), 0, bytes * 3);
    QVERIFY(view.attachChapter(makeChapter(1, QStringLiteral("第二章"), body), bytes, bytes * 3));
    // 还停在章首时，离视口太远的第三章会被卸掉。先滚到边界再接，才留得住。
    view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
    QVERIFY(view.attachChapter(makeChapter(2, QStringLiteral("第三章"), body), bytes * 2, bytes * 3));
    view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
    QCOMPARE(view.firstLoadedChapter(), 1U);
    QCOMPARE(view.chapterIndex(), 2U);
    QVERIFY(view.verticalScrollBar()->value() > 0);
}

void ReaderTest::momentumCrossesLoadedChaptersWithoutRequest()
{
    const QString body = tallBody(30, QStringLiteral("滑"));
    const quint64 bytes = static_cast<quint64>(body.toUtf8().size());
    moyu::ReaderView view;
    view.resize(320, 200);
    view.show();
    view.setChapterCount(3);
    view.setDocument(makeChapter(0, QStringLiteral("第一章"), body), 0, bytes * 3);
    QVERIFY(view.attachChapter(makeChapter(1, QStringLiteral("第二章"), body), bytes, bytes * 3));
    QVERIFY(view.attachChapter(makeChapter(2, QStringLiteral("第三章"), body), bytes * 2, bytes * 3));
    QVERIFY(view.verticalScrollBar()->maximum() > 0);

    int jumps = 0;
    QObject::connect(&view, &moyu::ReaderView::nextChapterRequested, &view, [&jumps] { ++jumps; });
    QObject::connect(&view, &moyu::ReaderView::previousChapterRequested, &view, [&jumps] { ++jumps; });
    // 字号不同时章高不一样。滚到大约五分之三，停在已加载内容中间，不撞到末尾。
    const int limit = std::max(1, view.verticalScrollBar()->maximum() * 3 / 5);
    int guard = 0;
    while (view.verticalScrollBar()->value() < limit && guard < 80) {
        QWheelEvent event(QPointF(20, 20),
                          QPointF(20, 20),
                          QPoint(0, -40),
                          QPoint(0, 0),
                          Qt::NoButton,
                          Qt::NoModifier,
                          Qt::ScrollMomentum,
                          false);
        QApplication::sendEvent(view.viewport(), &event);
        ++guard;
    }
    QVERIFY2(view.chapterIndex() > 0U,
             qPrintable(QStringLiteral("chapter %1 scroll %2 max %3 loaded %4")
                            .arg(view.chapterIndex())
                            .arg(view.verticalScrollBar()->value())
                            .arg(view.verticalScrollBar()->maximum())
                            .arg(view.loadedChapterCount())));
    QCOMPARE(jumps, 0);
}

void ReaderTest::pageModeLayoutsOnlyTheVisiblePage()
{
    const QString body = tallBody(80, QStringLiteral("页"));
    auto content = makeChapter(0, QStringLiteral("第一章"), body);
    const auto paragraphs = content.paragraphs.size();
    QVERIFY(paragraphs > 40U);

    moyu::ReaderView view;
    view.resize(280, 160);
    view.show();
    view.setDocument(std::move(content), 0, static_cast<quint64>(body.toUtf8().size()));
    view.setReadMode(moyu::ReadMode::Page);
    QVERIFY(view.laidOutParagraphs() > 0);
    QVERIFY(view.laidOutParagraphs() * 4 < static_cast<int>(paragraphs));
}

void ReaderTest::pageTurnSpansChapterBoundary()
{
    const QString first = QStringLiteral("甲\n乙\n尾\n");
    const QByteArray firstBytes = first.toUtf8();
    const qsizetype tail = firstBytes.indexOf(QStringLiteral("尾").toUtf8());
    QVERIFY(tail > 0);

    moyu::ReaderView view;
    view.resize(300, 220);
    view.show();
    view.setChapterCount(2);
    view.setDocument(makeChapter(0, QStringLiteral("第一章"), first), 0, 4000);
    view.scrollToByteOffset(static_cast<quint32>(tail));
    QVERIFY(view.attachChapter(makeChapter(1, QStringLiteral("第二章"), tallBody(40, QStringLiteral("下一章"))), 100, 4000));
    view.setReadMode(moyu::ReadMode::Page);
    QVERIFY(view.pageShowsChapter(0));
    QVERIFY(view.pageShowsChapter(1));

    int jumps = 0;
    QObject::connect(&view, &moyu::ReaderView::nextChapterRequested, &view, [&jumps] { ++jumps; });
    view.scrollPage(true);
    QCOMPARE(jumps, 0);
    QCOMPARE(view.chapterIndex(), 1U);
}

void ReaderTest::autoScrollRunsOnlyWhileEnabled()
{
    moyu::ReaderView view;
    view.resize(300, 200);
    view.show();
    const QString body = QStringLiteral("自动滚动的正文。\n");
    view.setDocument(makeChapter(0, QStringLiteral("第一章"), body), 0, static_cast<quint64>(body.toUtf8().size()));
    QVERIFY(!view.autoScrollRunning());
    view.setAutoScroll(true);
    QVERIFY(view.autoScrollRunning());
    view.pauseAutoScroll(true);
    QVERIFY(!view.autoScrollRunning());
    view.pauseAutoScroll(false);
    QVERIFY(view.autoScrollRunning());
    view.setAutoScroll(false);
    QVERIFY(!view.autoScrollRunning());
}

void ReaderTest::highlightPaintsTheKeyword()
{
    const QString text = QStringLiteral("这里有关键字需要高亮出来。\n");
    const QByteArray utf8 = text.toUtf8();
    const qsizetype at = utf8.indexOf(QStringLiteral("关键字").toUtf8());
    QVERIFY(at > 0);

    moyu::ReaderView view;
    view.resize(360, 160);
    view.show();
    view.setDocument(makeChapter(0, QStringLiteral("第一章"), text), 0, static_cast<quint64>(utf8.size()));
    view.setHighlight(0, static_cast<quint32>(at), static_cast<quint32>(QStringLiteral("关键字").toUtf8().size()));
    view.viewport()->repaint();

    const QImage image = view.viewport()->grab().toImage();
    bool gold = false;
    for (int y = 0; y < image.height() && !gold; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > 140 && pixel.red() > pixel.blue() + 40 && pixel.green() > pixel.blue()) {
                gold = true;
                break;
            }
        }
    }
    QVERIFY(gold);
}

void ReaderTest::preloadedNeighborIsAttached()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    moyu::Database database(dir.filePath(QStringLiteral("moyu.db")));
    QVERIFY(database.startAndWait());
    moyu::BookSession session(&database, dir.path());
    moyu::MainWindow window;
    window.resize(360, 240);
    QObject::connect(&session, &moyu::BookSession::bookReady, &window, [&window](const moyu::TxtDocument& document) {
        if (document.chapterIndex >= document.parsed.chapters.size()) {
            return;
        }
        const moyu::ChapterIndex& chapter = document.parsed.chapters[document.chapterIndex];
        window.showBook(document.parsed.chapters,
                        document.chapter,
                        chapter.byteStart,
                        document.parsed.book.totalBytes,
                        document.byteOffset);
    });
    QObject::connect(&session, &moyu::BookSession::neighborReady, &window, &moyu::MainWindow::attachNeighbor);

    QString text;
    for (int i = 1; i <= 3; ++i) {
        text += QStringLiteral("第%1章 标题\n正文一段，足够单独成章。\n").arg(i);
    }
    const QString path = dir.filePath(QStringLiteral("book.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(text.toUtf8()) > 0);
    file.close();

    session.openFile(path);
    QTRY_VERIFY(window.hasBook());
    QTRY_COMPARE(window.lastLoadedChapter(), 1U);
    QCOMPARE(window.firstLoadedChapter(), 0U);
    QCOMPARE(window.chapterIndex(), 0U);
}

void ReaderTest::bookmarkAddListAndDelete()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    moyu::Database database(dir.filePath(QStringLiteral("moyu.db")));
    QVERIFY(database.startAndWait());
    moyu::BookSession session(&database, dir.path());

    const QString path = dir.filePath(QStringLiteral("marks.txt"));
    const QByteArray text = QStringLiteral("第1章 甲\n你好。\n第2章 乙\n世界。\n").toUtf8();
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(text) == text.size());
    file.close();

    bool opened = false;
    QObject::connect(&session, &moyu::BookSession::bookReady, &session, [&opened](const moyu::TxtDocument&) { opened = true; });
    session.openFile(path);
    QTRY_VERIFY(opened);
    QVERIFY(session.bookId() > 0);

    session.addBookmark(1, 4, QStringLiteral("预览文字"));
    std::vector<moyu::Bookmark> rows;
    bool ready = false;
    QObject::connect(&session, &moyu::BookSession::bookmarksReady, &session, [&](const std::vector<moyu::Bookmark>& marks) {
        rows = marks;
        ready = true;
    });
    session.requestBookmarks();
    QTRY_VERIFY(ready);
    QCOMPARE(rows.size(), 1U);
    QCOMPARE(rows.front().chapterIndex, 1U);
    QCOMPARE(rows.front().byteOffsetInChapter, 4U);
    QCOMPARE(rows.front().preview, QStringLiteral("预览文字"));
    QVERIFY(rows.front().id > 0);

    const qint64 id = rows.front().id;
    ready = false;
    session.removeBookmark(id);
    session.requestBookmarks();
    QTRY_VERIFY(ready);
    QVERIFY(rows.empty());
}

void ReaderTest::scrollBarStaysHiddenAndHintTimerIsOneShot()
{
    // 系统「显示滚动条：始终」是进程启动时读的。这里直接断言滚动条控件保持隐藏。
    QString text;
    for (int i = 0; i < 80; ++i) {
        text += QStringLiteral("滚动条不应该出现的第%1段。\n").arg(i);
    }
    moyu::ReaderView view;
    view.resize(280, 160);
    view.show();
    view.setDocument(makeChapter(0, QStringLiteral("第一章"), text), 0, static_cast<quint64>(text.toUtf8().size()));
    QVERIFY(view.verticalScrollBar()->maximum() > view.viewport()->height());
    QCOMPARE(view.verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QVERIFY(!view.verticalScrollBar()->isVisible());
    for (QTimer* timer : view.findChildren<QTimer*>()) {
        QVERIFY2(!timer->isActive(), "刚打开时不应该有活动定时器");
    }

    view.resize(460, 280);
    QVERIFY(!view.verticalScrollBar()->isVisible());
    view.setReadMode(moyu::ReadMode::Page);
    QVERIFY(!view.verticalScrollBar()->isVisible());
    view.setReadMode(moyu::ReadMode::Scroll);
    view.resize(360, 200);
    QCOMPARE(view.verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QVERIFY(!view.verticalScrollBar()->isVisible());
    for (QTimer* timer : view.findChildren<QTimer*>()) {
        QVERIFY(!timer->isActive());
    }
    const auto countEdgeInk = [](const QImage& image) {
        int ink = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = std::max(0, image.width() - 6); x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() > 40 && pixel.red() > 180) {
                    ++ink;
                }
            }
        }
        return ink;
    };
    QCOMPARE(countEdgeInk(view.viewport()->grab().toImage()), 0);

    view.verticalScrollBar()->setValue(std::max(1, view.verticalScrollBar()->maximum() / 2));
    int active = 0;
    for (QTimer* timer : view.findChildren<QTimer*>()) {
        if (!timer->isActive()) {
            continue;
        }
        ++active;
        QVERIFY(timer->isSingleShot());
        QCOMPARE(timer->interval(), 1000);
    }
    QCOMPARE(active, 1);
    QVERIFY(!view.verticalScrollBar()->isVisible());
    QVERIFY(countEdgeInk(view.viewport()->grab().toImage()) > 0);

    QTest::qWait(1100);
    for (QTimer* timer : view.findChildren<QTimer*>()) {
        QVERIFY2(!timer->isActive(), "停下约 1 秒后进度指示应该淡出");
    }
    QCOMPARE(countEdgeInk(view.viewport()->grab().toImage()), 0);
    view.resize(400, 220);
    QVERIFY(!view.verticalScrollBar()->isVisible());
}

void ReaderTest::paintOfHundredMegabyteBook()
{
    QString path;
    const QStringList candidates = {QStringLiteral("../../bench/data/utf8_100mb.txt"),
                                    QStringLiteral("../bench/data/utf8_100mb.txt"),
                                    QStringLiteral("bench/data/utf8_100mb.txt")};
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            path = candidate;
            break;
        }
    }
    if (path.isEmpty()) {
        QSKIP("没有 bench/data/utf8_100mb.txt，跳过绘制耗时");
    }

    auto mapped = moyu::MappedFile::open(path);
    QVERIFY(mapped);
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(mapped->data(), mapped->size(), &cancel);
    QVERIFY(chapters);
    QVERIFY(chapters->size() > 10U);
    const moyu::ChapterIndex& chapter = chapters->at(chapters->size() / 2);
    auto content = moyu::makeChapterContent(chapter, mapped->data(), mapped->size());
    QVERIFY(content.paragraphs.size() > 20U);

    moyu::ReaderView view;
    view.resize(800, 600);
    view.show();
    view.setDocument(std::move(content), chapter.byteStart, mapped->size());
    QVERIFY(view.verticalScrollBar()->maximum() > 0);

    std::vector<qint64> samples;
    view.setPaintProbe([&samples](qint64 nsec) { samples.push_back(nsec); });
    constexpr int kFrames = 120;
    for (int i = 0; i < kFrames; ++i) {
        const int maximum = view.verticalScrollBar()->maximum();
        const int value = maximum * i / (kFrames - 1);
        view.verticalScrollBar()->setValue(value);
        // grab 会走 viewportEvent，QPainter 才有画布。repaint 只打到 viewport 自己的空 paintEvent。
        const QImage frame = view.viewport()->grab().toImage();
        QVERIFY(!frame.isNull());
    }
    QVERIFY2(samples.size() >= static_cast<std::size_t>(kFrames),
             qPrintable(QStringLiteral("samples %1").arg(samples.size())));
    std::sort(samples.begin(), samples.end());
    const auto at = [&samples](int percent) {
        const std::size_t index = (samples.size() - 1) * static_cast<std::size_t>(percent) / 100;
        return samples[index];
    };
    const double p50 = static_cast<double>(at(50)) / 1.0e6;
    const double p99 = static_cast<double>(at(99)) / 1.0e6;
    std::fprintf(stderr, "paint_100mb_p50_ms %.3f p99_ms %.3f samples %zu\n", p50, p99, samples.size());
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    ReaderTest tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_reader.moc"
