#pragma once

#include "core/Types.h"

#include <QThread>

#include <functional>
#include <utility>
#include <vector>

class QObject;

namespace moyu {

class DatabaseWorker;

// 全部 SQL 都在这条线程上。调用方自己不碰连接。
// 加载线程上的查询用阻塞调用等结果。UI 上的写入只排队，不等执行完。
// 读设置走 requestSettings，结果回到 context 所在的线程。
// 必须等 startAndWait() 返回之后再调用，否则目标线程还没有事件循环，会互相等死。
class Database {
public:
    explicit Database(QString path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool startAndWait();
    bool isOpen() const { return m_open; }
    QString errorString() const { return m_error; }

    Result<std::optional<StoredBook>> findByFingerprint(const QString& token);
    Result<std::optional<StoredBook>> findByPath(const QString& path);
    Result<std::optional<StoredBook>> loadLastSession();
    Result<std::vector<ShelfEntry>> listShelf();
    // cacheDir 用来删 UTF-8 缓存。原文件不会动。
    Result<void> deleteBook(qint64 id, const QString& cacheDir);
    Result<void> retargetBook(qint64 id, const QString& newPath);
    Result<qint64> upsertBook(const Book& book);
    Result<void> replaceChapters(qint64 bookId, const std::vector<ChapterIndex>& chapters);
    Result<void> saveProgress(const ReadingProgress& progress);
    Result<QString> setting(const QString& key);
    Result<void> setSettings(const std::vector<std::pair<QString, QString>>& items);
    Result<qint64> addBookmark(const Bookmark& bookmark);
    Result<void> deleteBookmark(qint64 id);
    Result<std::vector<Bookmark>> bookmarks(qint64 bookId);

    // 排队即返回。数据库线程正写着几千条章节时，界面不用等那次写结束。
    bool saveProgressAsync(const ReadingProgress& progress);
    bool setSettingsAsync(const std::vector<std::pair<QString, QString>>& items);
    bool addBookmarkAsync(const Bookmark& bookmark);
    bool deleteBookmarkAsync(qint64 id);
    void requestSettings(QObject* context,
                         const std::vector<QString>& keys,
                         const std::function<void(const std::vector<std::pair<QString, QString>>&)>& callback);
    void requestBookmarks(QObject* context,
                          qint64 bookId,
                          const std::function<void(const std::vector<Bookmark>&)>& callback);

private:
    QString m_path;
    QThread m_thread;
    DatabaseWorker* m_worker = nullptr;
    bool m_open = false;
    QString m_error;
};

}  // namespace moyu
