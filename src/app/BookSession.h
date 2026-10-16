#pragma once

#include "core/Types.h"
#include "parser/TxtParser.h"

#include <QObject>
#include <QRect>
#include <QStringList>

#include <atomic>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

class QThread;
class QTimer;

namespace moyu {

class Database;

struct WindowPlacement {
    QRect geometry;
    bool hasGeometry = false;
    bool onTop = false;
    bool hasOnTop = false;
};

struct SearchHitInfo {
    quint32 chapterIndex = 0;
    quint32 byteOffset = 0;
    QString preview;
};

// UI 线程上的阅读会话。文件和 SQL 都不在 UI 线程做：
// 一个加载线程按顺序处理打开请求，一条数据库线程执行 SQL。
// openBookFile 把 (0, 0) 当成真实进度，所以恢复上次位置由这里把提示传进去。
class BookSession : public QObject {
    Q_OBJECT

public:
    BookSession(Database* database, QString cacheDir, QObject* parent = nullptr);
    ~BookSession() override;

    BookSession(const BookSession&) = delete;
    BookSession& operator=(const BookSession&) = delete;

    void openFile(const QString& path);
    void openFiles(const QStringList& paths);
    void openLast();
    void setChapterPattern(const QString& pattern);
    void regenerateChapters();
    void removeFromShelf(qint64 id);
    void goToChapter(quint32 index, bool pinToEnd);
    void goToAnchor(quint32 index, quint32 offset);
    void requestNeighbor(quint32 index);
    void search(const QString& query);
    void cancelSearch();
    void addBookmark(quint32 chapter, quint32 offset, const QString& preview);
    void removeBookmark(qint64 id);
    void requestBookmarks();
    qint64 bookId() const;
    void noteAnchor(quint32 chapterIndex, quint32 byteOffset);
    void noteGeometry(QRect geometry);
    void setAlwaysOnTop(bool on);
    void flushNow();
    void requestPlacement();

Q_SIGNALS:
    void previewReady(ChapterContent content, quint64 bodyByteStart, quint64 totalBytes);
    void bookReady(TxtDocument document);
    void chapterReady(ChapterContent content, quint64 chapterByteStart, quint64 totalBytes, quint32 byteOffset, bool pinToEnd);
    void neighborReady(ChapterContent content, quint64 chapterByteStart, quint64 totalBytes);
    void searchBatch(std::vector<SearchHitInfo> hits, bool done);
    void bookmarksReady(std::vector<Bookmark> bookmarks);
    void openFailed(QString message, bool clearPreview);
    void fileMissing(qint64 bookId, QString title, QString path);
    void bookCleared();
    void shelfChanged();
    void placementReady(WindowPlacement placement);

private:
    struct Loader;
    struct Job;

    void enqueue(const Job& job);
    void deliverBook(const TxtDocument& document);
    void deliverChapter(ChapterContent content, quint32 index, quint32 byteOffset, bool pinToEnd);
    void deliverNeighbor(ChapterContent content, quint32 index);
    void preloadAround(quint32 index);
    void remember(quint32 index, const ChapterContent& content);
    void flushProgress();
    void flushSettings();
    bool restoreAfterFailedOpen();

    Database* m_database = nullptr;
    QString m_cacheDir;
    // 每个打开任务拿自己的取消标记。不能先看代号再把共享标记清成 false，中间会把新任务的取消盖掉。
    std::shared_ptr<std::atomic<bool>> m_activeCancel = std::make_shared<std::atomic<bool>>(false);
    std::atomic<quint64> m_generation{0};
    std::atomic<quint64> m_viewToken{0};
    std::unique_ptr<QThread> m_thread;
    Loader* m_loader = nullptr;

    QTimer* m_progressTimer = nullptr;
    QTimer* m_geometryTimer = nullptr;

    std::optional<ParsedBook> m_book;
    std::unordered_map<quint32, ChapterContent> m_cache;
    std::unordered_set<quint32> m_inflight;
    std::shared_ptr<std::atomic<bool>> m_searchCancel;
    std::shared_ptr<std::atomic<int>> m_searchJobs = std::make_shared<std::atomic<int>>(0);
    quint32 m_currentIndex = 0;
    quint32 m_savedChapter = 0;
    quint32 m_savedOffset = 0;
    quint32 m_pendingChapter = 0;
    quint32 m_pendingOffset = 0;
    bool m_progressDirty = false;
    bool m_suspendAnchor = false;
    bool m_previewPosted = false;

    QString m_chapterPattern;
    QRect m_geometry;
    bool m_geometryDirty = false;
    bool m_onTop = false;
    bool m_onTopDirty = false;
};

}  // namespace moyu
