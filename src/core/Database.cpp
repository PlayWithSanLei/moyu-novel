#include "core/Database.h"

#include "core/Paths.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <utility>

namespace moyu {
namespace {

constexpr auto kSchemaBooks =
    "CREATE TABLE IF NOT EXISTS books ("
    "id INTEGER PRIMARY KEY,"
    "file_path TEXT NOT NULL,"
    "fingerprint TEXT NOT NULL UNIQUE,"
    "title TEXT NOT NULL,"
    "author TEXT NOT NULL DEFAULT '',"
    "format INTEGER NOT NULL,"
    "source_encoding TEXT NOT NULL,"
    "total_bytes INTEGER NOT NULL,"
    "last_read_at INTEGER NOT NULL)";

constexpr auto kSchemaChapters =
    "CREATE TABLE IF NOT EXISTS chapters ("
    "book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE,"
    "idx INTEGER NOT NULL,"
    "title TEXT NOT NULL,"
    "level INTEGER NOT NULL,"
    "byte_start INTEGER NOT NULL,"
    "byte_end INTEGER NOT NULL,"
    "synthetic INTEGER NOT NULL,"
    "PRIMARY KEY (book_id, idx))";

constexpr auto kSchemaProgress =
    "CREATE TABLE IF NOT EXISTS progress ("
    "book_id INTEGER PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE,"
    "chapter_index INTEGER NOT NULL,"
    "byte_offset INTEGER NOT NULL,"
    "updated_at INTEGER NOT NULL)";

constexpr auto kSchemaBookmarks =
    "CREATE TABLE IF NOT EXISTS bookmarks ("
    "id INTEGER PRIMARY KEY,"
    "book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE,"
    "chapter_index INTEGER NOT NULL,"
    "byte_offset INTEGER NOT NULL,"
    "preview TEXT NOT NULL,"
    "created_at INTEGER NOT NULL)";

constexpr auto kSchemaSettings =
    "CREATE TABLE IF NOT EXISTS settings ("
    "\"key\" TEXT PRIMARY KEY,"
    "value TEXT NOT NULL)";

bool execSql(QSqlDatabase& database, const char* sql)
{
    QSqlQuery query(database);
    return query.exec(QString::fromUtf8(sql));
}

qint64 asSigned(quint64 value, bool* ok)
{
    if (value > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        *ok = false;
        return 0;
    }
    *ok = true;
    return static_cast<qint64>(value);
}

Book bookFromQuery(const QSqlQuery& query)
{
    Book book;
    book.id = query.value(0).toLongLong();
    book.filePath = query.value(1).toString();
    book.fingerprint = query.value(2).toString();
    book.title = query.value(3).toString();
    book.author = query.value(4).toString();
    book.format = static_cast<BookFormat>(query.value(5).toInt());
    book.sourceEncoding = query.value(6).toString();
    const qint64 bytes = query.value(7).toLongLong();
    book.totalBytes = bytes > 0 ? static_cast<quint64>(bytes) : 0;
    book.lastReadAt = query.value(8).toLongLong();
    return book;
}

}  // namespace

class DatabaseWorker : public QObject {
    Q_OBJECT

public:
    void openAt(const QString& path)
    {
        // 强制退出时未 checkpoint 的页留在 -wal 里。下次打开会先重放，这里记下大小。
        // QFileInfo::size() 在文件还没被这个进程碰过时会回 0，改用 stat。
        const QByteArray walPath = (path + QStringLiteral("-wal")).toUtf8();
        struct stat walStat {};
        const qint64 walBytes = ::stat(walPath.constData(), &walStat) == 0 ? static_cast<qint64>(walStat.st_size) : 0;
        QElapsedTimer openTimer;
        openTimer.start();
        const auto finish = [&](bool ok, const QString& error) {
            if (qgetenv("MOYU_TIMING") == "1") {
                const double ms = static_cast<double>(openTimer.nsecsElapsed()) / 1.0e6;
                std::fprintf(stderr, "moyu_db_open_ms %.3f wal_bytes %lld\n", ms, static_cast<long long>(walBytes));
                std::fflush(stderr);
            }
            Q_EMIT opened(ok, error);
        };

        QDir().mkpath(QFileInfo(path).absolutePath());
        m_connection = QStringLiteral("moyu-%1").arg(reinterpret_cast<quintptr>(this));
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
        m_db.setDatabaseName(path);
        if (!m_db.open()) {
            finish(false, m_db.lastError().text());
            return;
        }
        if (!execSql(m_db, "PRAGMA journal_mode=WAL") || !execSql(m_db, "PRAGMA synchronous=NORMAL") ||
            !execSql(m_db, "PRAGMA foreign_keys=ON") || !execSql(m_db, "PRAGMA busy_timeout=2000") ||
            !execSql(m_db, kSchemaBooks) || !execSql(m_db, kSchemaChapters) || !execSql(m_db, kSchemaProgress) ||
            !execSql(m_db, kSchemaBookmarks) || !execSql(m_db, kSchemaSettings)) {
            const QString message = m_db.lastError().text();
            finish(false, message.isEmpty() ? QStringLiteral("无法初始化数据库") : message);
            return;
        }
        finish(true, QString());
    }

    void shutdown()
    {
        const QString name = m_db.connectionName();
        if (m_db.isOpen()) {
            // 正常退出时把 WAL 收进主库。强制结束赶不上这一步，下次打开会重放剩下的页。
            execSql(m_db, "PRAGMA wal_checkpoint(TRUNCATE)");
            m_db.close();
        }
        m_db = QSqlDatabase();
        if (!name.isEmpty()) {
            QSqlDatabase::removeDatabase(name);
        }
        // 线程停掉之后，调用方要在原来的线程上 delete。先把亲和性拿掉。
        moveToThread(nullptr);
    }

    Result<std::optional<StoredBook>> findByFingerprint(const QString& token)
    {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "SELECT id, file_path, fingerprint, title, author, format, source_encoding, total_bytes, last_read_at "
            "FROM books WHERE fingerprint=?"));
        query.bindValue(0, token);
        if (!query.exec()) {
            return std::unexpected(makeError(query.lastError().text()));
        }
        if (!query.next()) {
            return std::optional<StoredBook>{};
        }
        return takeStored(bookFromQuery(query));
    }

    Result<std::optional<StoredBook>> findByPath(const QString& path)
    {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "SELECT id, file_path, fingerprint, title, author, format, source_encoding, total_bytes, last_read_at "
            "FROM books WHERE file_path=? ORDER BY last_read_at DESC LIMIT 1"));
        query.bindValue(0, path);
        if (!query.exec()) {
            return std::unexpected(makeError(query.lastError().text()));
        }
        if (!query.next()) {
            return std::optional<StoredBook>{};
        }
        return takeStored(bookFromQuery(query));
    }

    Result<std::optional<StoredBook>> loadLastSession()
    {
        QSqlQuery query(m_db);
        if (!query.exec(QStringLiteral(
                "SELECT id, file_path, fingerprint, title, author, format, source_encoding, total_bytes, last_read_at "
                "FROM books ORDER BY last_read_at DESC LIMIT 1"))) {
            return std::unexpected(makeError(query.lastError().text()));
        }
        if (!query.next()) {
            return std::optional<StoredBook>{};
        }
        return takeStored(bookFromQuery(query));
    }

    Result<qint64> upsertBook(const Book& book)
    {
        bool bytesOk = false;
        const qint64 total = asSigned(book.totalBytes, &bytesOk);
        if (!bytesOk) {
            return std::unexpected(makeError(QStringLiteral("文件太大")));
        }
        // 空 QString 是 null。Qt 会把它绑成 SQL NULL，NOT NULL 列就会拒绝。
        const auto text = [](const QString& value) {
            return value.isNull() ? QStringLiteral("") : value;
        };
        const QString path = text(book.filePath);
        const QString fingerprint = text(book.fingerprint);

        // 同一路径只留一行。连载更新后指纹变了，沿用原来的 id，进度不会被新行丢掉。
        qint64 existingId = -1;
        {
            QSqlQuery byPath(m_db);
            byPath.prepare(QStringLiteral(
                "SELECT id FROM books WHERE file_path=? ORDER BY last_read_at DESC LIMIT 1"));
            byPath.bindValue(0, path);
            if (!byPath.exec()) {
                return std::unexpected(makeError(byPath.lastError().text().isEmpty()
                                                      ? QStringLiteral("无法保存书籍")
                                                      : byPath.lastError().text()));
            }
            if (byPath.next()) {
                existingId = byPath.value(0).toLongLong();
            }
        }
        if (existingId >= 0) {
            if (!m_db.transaction()) {
                return std::unexpected(makeError(m_db.lastError().text()));
            }
            QSqlQuery dropPath(m_db);
            dropPath.prepare(QStringLiteral("DELETE FROM books WHERE file_path=? AND id<>?"));
            dropPath.bindValue(0, path);
            dropPath.bindValue(1, existingId);
            QSqlQuery dropFingerprint(m_db);
            dropFingerprint.prepare(QStringLiteral("DELETE FROM books WHERE fingerprint=? AND id<>?"));
            dropFingerprint.bindValue(0, fingerprint);
            dropFingerprint.bindValue(1, existingId);
            QSqlQuery update(m_db);
            update.prepare(QStringLiteral(
                "UPDATE books SET fingerprint=?, title=?, author=?, format=?, source_encoding=?, "
                "total_bytes=?, last_read_at=? WHERE id=?"));
            update.bindValue(0, fingerprint);
            update.bindValue(1, text(book.title));
            update.bindValue(2, text(book.author));
            update.bindValue(3, static_cast<int>(book.format));
            update.bindValue(4, text(book.sourceEncoding));
            update.bindValue(5, total);
            update.bindValue(6, book.lastReadAt);
            update.bindValue(7, existingId);
            if (!dropPath.exec() || !dropFingerprint.exec() || !update.exec() || !m_db.commit()) {
                m_db.rollback();
                const QString message = update.lastError().text().isEmpty() ? m_db.lastError().text()
                                                                              : update.lastError().text();
                return std::unexpected(makeError(message.isEmpty() ? QStringLiteral("无法保存书籍") : message));
            }
            return existingId;
        }

        QSqlQuery query(m_db);
        // Qt 的 QSQLITE 把 INSERT 当成非查询。RETURNING 即使给出一行，next() 也拿不到。
        // 插完再按指纹查 id，冲突更新和首次插入走同一条路径。
        query.prepare(QStringLiteral(
            "INSERT INTO books (file_path, fingerprint, title, author, format, source_encoding, total_bytes, last_read_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(fingerprint) DO UPDATE SET "
            "file_path=excluded.file_path, title=excluded.title, author=excluded.author, "
            "format=excluded.format, source_encoding=excluded.source_encoding, "
            "total_bytes=excluded.total_bytes, last_read_at=excluded.last_read_at"));
        query.bindValue(0, path);
        query.bindValue(1, fingerprint);
        query.bindValue(2, text(book.title));
        query.bindValue(3, text(book.author));
        query.bindValue(4, static_cast<int>(book.format));
        query.bindValue(5, text(book.sourceEncoding));
        query.bindValue(6, total);
        query.bindValue(7, book.lastReadAt);
        if (!query.exec()) {
            return std::unexpected(makeError(query.lastError().text().isEmpty()
                                                  ? QStringLiteral("无法保存书籍")
                                                  : query.lastError().text()));
        }
        QSqlQuery lookup(m_db);
        lookup.prepare(QStringLiteral("SELECT id FROM books WHERE fingerprint=?"));
        lookup.bindValue(0, fingerprint);
        if (!lookup.exec() || !lookup.next()) {
            return std::unexpected(makeError(lookup.lastError().text().isEmpty()
                                                  ? QStringLiteral("无法保存书籍")
                                                  : lookup.lastError().text()));
        }
        return lookup.value(0).toLongLong();
    }

    Result<void> replaceChapters(qint64 bookId, const std::vector<ChapterIndex>& chapters)
    {
        if (!m_db.transaction()) {
            return std::unexpected(makeError(m_db.lastError().text()));
        }
        QSqlQuery remove(m_db);
        remove.prepare(QStringLiteral("DELETE FROM chapters WHERE book_id=?"));
        remove.bindValue(0, bookId);
        if (!remove.exec()) {
            m_db.rollback();
            return std::unexpected(makeError(remove.lastError().text()));
        }
        QSqlQuery insert(m_db);
        insert.prepare(QStringLiteral(
            "INSERT INTO chapters (book_id, idx, title, level, byte_start, byte_end, synthetic) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)"));
        for (const ChapterIndex& chapter : chapters) {
            bool startOk = false;
            bool endOk = false;
            const qint64 start = asSigned(chapter.byteStart, &startOk);
            const qint64 end = asSigned(chapter.byteEnd, &endOk);
            if (!startOk || !endOk) {
                m_db.rollback();
                return std::unexpected(makeError(QStringLiteral("章节偏移超出范围")));
            }
            insert.bindValue(0, bookId);
            insert.bindValue(1, static_cast<qint64>(chapter.index));
            insert.bindValue(2, chapter.title.isNull() ? QStringLiteral("") : chapter.title);
            insert.bindValue(3, static_cast<int>(chapter.level));
            insert.bindValue(4, start);
            insert.bindValue(5, end);
            insert.bindValue(6, chapter.synthetic ? 1 : 0);
            if (!insert.exec()) {
                m_db.rollback();
                return std::unexpected(makeError(insert.lastError().text()));
            }
        }
        if (!m_db.commit()) {
            return std::unexpected(makeError(m_db.lastError().text()));
        }
        // 章节写完就 checkpoint。之后再被 kill -9，主库里已经有目录，不用整段重放。
        execSql(m_db, "PRAGMA wal_checkpoint(PASSIVE)");
        return {};
    }

    Result<void> saveProgress(const ReadingProgress& progress)
    {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "INSERT INTO progress (book_id, chapter_index, byte_offset, updated_at) "
            "VALUES (?, ?, ?, ?) "
            "ON CONFLICT(book_id) DO UPDATE SET "
            "chapter_index=excluded.chapter_index, byte_offset=excluded.byte_offset, "
            "updated_at=excluded.updated_at"));
        query.bindValue(0, progress.bookId);
        query.bindValue(1, static_cast<qint64>(progress.chapterIndex));
        query.bindValue(2, static_cast<qint64>(progress.byteOffsetInChapter));
        query.bindValue(3, progress.updatedAt);
        if (!query.exec()) {
            return std::unexpected(makeError(query.lastError().text()));
        }
        QSqlQuery touch(m_db);
        touch.prepare(QStringLiteral("UPDATE books SET last_read_at=? WHERE id=?"));
        touch.bindValue(0, progress.updatedAt);
        touch.bindValue(1, progress.bookId);
        if (!touch.exec()) {
            return std::unexpected(makeError(touch.lastError().text()));
        }
        return {};
    }

    Result<QString> setting(const QString& key)
    {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral("SELECT value FROM settings WHERE \"key\"=?"));
        query.bindValue(0, key);
        if (!query.exec()) {
            return std::unexpected(makeError(query.lastError().text()));
        }
        if (!query.next()) {
            return QString();
        }
        return query.value(0).toString();
    }

    Result<void> setSettings(const std::vector<std::pair<QString, QString>>& items)
    {
        if (!m_db.transaction()) {
            return std::unexpected(makeError(m_db.lastError().text()));
        }
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "INSERT INTO settings (\"key\", value) VALUES (?, ?) "
            "ON CONFLICT(\"key\") DO UPDATE SET value=excluded.value"));
        for (const auto& item : items) {
            query.bindValue(0, item.first);
            query.bindValue(1, item.second);
            if (!query.exec()) {
                m_db.rollback();
                return std::unexpected(makeError(query.lastError().text()));
            }
        }
        if (!m_db.commit()) {
            return std::unexpected(makeError(m_db.lastError().text()));
        }
        return {};
    }

    Result<qint64> addBookmark(const Bookmark& bookmark)
    {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "INSERT INTO bookmarks (book_id, chapter_index, byte_offset, preview, created_at) "
            "VALUES (?, ?, ?, ?, ?)"));
        query.bindValue(0, bookmark.bookId);
        query.bindValue(1, static_cast<qint64>(bookmark.chapterIndex));
        query.bindValue(2, static_cast<qint64>(bookmark.byteOffsetInChapter));
        query.bindValue(3, bookmark.preview.isNull() ? QStringLiteral("") : bookmark.preview);
        query.bindValue(4, bookmark.createdAt);
        if (!query.exec()) {
            return std::unexpected(makeError(query.lastError().text().isEmpty() ? QStringLiteral("无法保存书签")
                                                                                  : query.lastError().text()));
        }
        QSqlQuery idQuery(m_db);
        if (!idQuery.exec(QStringLiteral("SELECT last_insert_rowid()")) || !idQuery.next()) {
            return std::unexpected(makeError(QStringLiteral("无法保存书签")));
        }
        return idQuery.value(0).toLongLong();
    }

    Result<void> deleteBookmark(qint64 id)
    {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral("DELETE FROM bookmarks WHERE id=?"));
        query.bindValue(0, id);
        if (!query.exec()) {
            return std::unexpected(makeError(query.lastError().text()));
        }
        return {};
    }

    Result<std::vector<Bookmark>> bookmarks(qint64 bookId)
    {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "SELECT id, book_id, chapter_index, byte_offset, preview, created_at "
            "FROM bookmarks WHERE book_id=? ORDER BY created_at DESC, id DESC"));
        query.bindValue(0, bookId);
        if (!query.exec()) {
            return std::unexpected(makeError(query.lastError().text()));
        }
        std::vector<Bookmark> rows;
        while (query.next()) {
            Bookmark mark;
            mark.id = query.value(0).toLongLong();
            mark.bookId = query.value(1).toLongLong();
            mark.chapterIndex = static_cast<quint32>(query.value(2).toULongLong());
            mark.byteOffsetInChapter = static_cast<quint32>(query.value(3).toULongLong());
            mark.preview = query.value(4).toString();
            mark.createdAt = query.value(5).toLongLong();
            rows.push_back(std::move(mark));
        }
        return rows;
    }

Q_SIGNALS:
    void opened(bool ok, QString error);

private:
    Result<std::optional<StoredBook>> takeStored(Book book)
    {
        auto stored = loadStored(std::move(book));
        if (!stored) {
            return std::unexpected(stored.error());
        }
        return std::optional<StoredBook>{std::move(*stored)};
    }

    Result<StoredBook> loadStored(Book book)
    {
        StoredBook stored;
        stored.book = std::move(book);
        QSqlQuery chapters(m_db);
        chapters.prepare(QStringLiteral(
            "SELECT idx, title, level, byte_start, byte_end, synthetic FROM chapters "
            "WHERE book_id=? ORDER BY idx"));
        chapters.bindValue(0, stored.book.id);
        if (!chapters.exec()) {
            return std::unexpected(makeError(chapters.lastError().text()));
        }
        while (chapters.next()) {
            ChapterIndex chapter;
            const qint64 index = chapters.value(0).toLongLong();
            const qint64 start = chapters.value(3).toLongLong();
            const qint64 end = chapters.value(4).toLongLong();
            if (index < 0 || start < 0 || end < 0) {
                return std::unexpected(makeError(QStringLiteral("章节索引损坏")));
            }
            chapter.index = static_cast<quint32>(index);
            chapter.title = chapters.value(1).toString();
            chapter.level = static_cast<quint8>(chapters.value(2).toInt());
            chapter.byteStart = static_cast<quint64>(start);
            chapter.byteEnd = static_cast<quint64>(end);
            chapter.synthetic = chapters.value(5).toInt() != 0;
            stored.chapters.push_back(std::move(chapter));
        }

        QSqlQuery progress(m_db);
        progress.prepare(QStringLiteral(
            "SELECT chapter_index, byte_offset, updated_at FROM progress WHERE book_id=?"));
        progress.bindValue(0, stored.book.id);
        if (!progress.exec()) {
            return std::unexpected(makeError(progress.lastError().text()));
        }
        if (progress.next()) {
            const qint64 index = progress.value(0).toLongLong();
            const qint64 offset = progress.value(1).toLongLong();
            if (index < 0 || offset < 0) {
                return std::unexpected(makeError(QStringLiteral("阅读进度损坏")));
            }
            ReadingProgress row;
            row.bookId = stored.book.id;
            row.chapterIndex = static_cast<quint32>(index);
            row.byteOffsetInChapter = static_cast<quint32>(std::min<qint64>(offset, std::numeric_limits<quint32>::max()));
            row.updatedAt = progress.value(2).toLongLong();
            stored.progress = row;
        }
        return stored;
    }

public:
    Result<std::vector<ShelfEntry>> listShelf()
    {
        QSqlQuery query(m_db);
        if (!query.exec(QStringLiteral(
                "SELECT b.id, b.title, b.file_path, b.fingerprint, b.last_read_at, b.total_bytes, "
                "p.byte_offset, c.byte_start, c.byte_end "
                "FROM books b "
                "LEFT JOIN progress p ON p.book_id = b.id "
                "LEFT JOIN chapters c ON c.book_id = b.id AND c.idx = p.chapter_index "
                "ORDER BY b.last_read_at DESC, b.id DESC"))) {
            return std::unexpected(makeError(query.lastError().text().isEmpty() ? QStringLiteral("无法读取书架")
                                                                                  : query.lastError().text()));
        }
        std::vector<ShelfEntry> rows;
        while (query.next()) {
            ShelfEntry entry;
            entry.id = query.value(0).toLongLong();
            entry.title = query.value(1).toString();
            entry.filePath = query.value(2).toString();
            entry.fingerprint = query.value(3).toString();
            entry.lastReadAt = query.value(4).toLongLong();
            const qint64 totalSigned = query.value(5).toLongLong();
            const quint64 total = totalSigned > 0 ? static_cast<quint64>(totalSigned) : 0;
            const bool hasProgress = !query.value(6).isNull() && !query.value(7).isNull() && !query.value(8).isNull();
            if (hasProgress && total > 0) {
                const qint64 startSigned = query.value(7).toLongLong();
                const qint64 endSigned = query.value(8).toLongLong();
                const qint64 offsetSigned = query.value(6).toLongLong();
                if (startSigned >= 0 && endSigned >= startSigned && offsetSigned >= 0) {
                    const quint64 start = static_cast<quint64>(startSigned);
                    const quint64 end = static_cast<quint64>(endSigned);
                    quint64 offset = static_cast<quint64>(offsetSigned);
                    const quint64 span = end - start;
                    if (offset > span) {
                        offset = span;
                    }
                    quint64 position = start + offset;
                    if (position > total) {
                        position = total;
                    }
                    entry.percent = static_cast<int>((position * 100) / total);
                }
            }
            rows.push_back(std::move(entry));
        }
        return rows;
    }

    Result<void> deleteBook(qint64 id, const QString& cacheDir)
    {
        QSqlQuery lookup(m_db);
        lookup.prepare(QStringLiteral("SELECT fingerprint, file_path FROM books WHERE id=?"));
        lookup.bindValue(0, id);
        if (!lookup.exec()) {
            return std::unexpected(makeError(lookup.lastError().text().isEmpty() ? QStringLiteral("无法删除书籍")
                                                                                   : lookup.lastError().text()));
        }
        if (!lookup.next()) {
            return std::unexpected(makeError(QStringLiteral("书架上没有这本书")));
        }
        const QString fingerprint = lookup.value(0).toString();
        const QString sourcePath = lookup.value(1).toString();
        QSqlQuery remove(m_db);
        remove.prepare(QStringLiteral("DELETE FROM books WHERE id=?"));
        remove.bindValue(0, id);
        if (!remove.exec()) {
            return std::unexpected(makeError(remove.lastError().text().isEmpty() ? QStringLiteral("无法删除书籍")
                                                                                   : remove.lastError().text()));
        }
        // 只删缓存。原文件留在用户自己的目录里。
        if (!cacheDir.isEmpty() && !fingerprint.isEmpty()) {
            const QString cachePath = utf8CachePath(cacheDir, fingerprint);
            if (cachePath != sourcePath) {
                QFile::remove(cachePath);
                QFile::remove(cachePath + QStringLiteral(".partial"));
            }
        }
        return {};
    }

    Result<void> retargetBook(qint64 id, const QString& newPath)
    {
        if (newPath.isEmpty()) {
            return std::unexpected(makeError(QStringLiteral("找不到文件")));
        }
        QSqlQuery conflict(m_db);
        conflict.prepare(QStringLiteral("SELECT id FROM books WHERE file_path=? AND id<>?"));
        conflict.bindValue(0, newPath);
        conflict.bindValue(1, id);
        if (!conflict.exec()) {
            return std::unexpected(makeError(conflict.lastError().text().isEmpty() ? QStringLiteral("无法更新路径")
                                                                                     : conflict.lastError().text()));
        }
        if (conflict.next()) {
            return std::unexpected(makeError(QStringLiteral("书架上已经有这个文件")));
        }
        QSqlQuery update(m_db);
        update.prepare(QStringLiteral("UPDATE books SET file_path=? WHERE id=?"));
        update.bindValue(0, newPath);
        update.bindValue(1, id);
        if (!update.exec()) {
            return std::unexpected(makeError(update.lastError().text().isEmpty() ? QStringLiteral("无法更新路径")
                                                                                   : update.lastError().text()));
        }
        if (update.numRowsAffected() == 0) {
            return std::unexpected(makeError(QStringLiteral("书架上没有这本书")));
        }
        return {};
    }

private:
    QString m_connection;
    QSqlDatabase m_db;
};

template <typename F>
auto runOnWorker(DatabaseWorker* worker, QThread* thread, F&& function) -> std::invoke_result_t<F>
{
    using R = std::invoke_result_t<F>;
    if (QThread::currentThread() == thread) {
        return function();
    }
    std::conditional_t<std::is_void_v<R>, int, R> result{};
    const bool invoked = QMetaObject::invokeMethod(
        worker,
        [&] {
            if constexpr (std::is_void_v<R>) {
                function();
            } else {
                result = function();
            }
        },
        Qt::BlockingQueuedConnection);
    if (!invoked) {
        if constexpr (!std::is_void_v<R>) {
            return std::unexpected(makeError(QStringLiteral("数据库线程没有响应")));
        }
    }
    if constexpr (!std::is_void_v<R>) {
        return result;
    }
}

Database::Database(QString path)
    : m_path(std::move(path))
    , m_worker(new DatabaseWorker())
{
    m_worker->moveToThread(&m_thread);
    m_thread.setObjectName(QStringLiteral("moyu-db"));
    m_thread.start();
}

Database::~Database()
{
    if (m_thread.isRunning() && m_worker != nullptr) {
        QMetaObject::invokeMethod(m_worker, &DatabaseWorker::shutdown, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
    delete m_worker;
    m_worker = nullptr;
}

bool Database::startAndWait()
{
    QEventLoop loop;
    QObject::connect(
        m_worker,
        &DatabaseWorker::opened,
        &loop,
        [this, &loop](bool ok, const QString& error) {
            m_open = ok;
            m_error = error;
            loop.quit();
        },
        Qt::QueuedConnection);
    const QString path = m_path;
    QMetaObject::invokeMethod(
        m_worker, [this, path] { m_worker->openAt(path); }, Qt::QueuedConnection);
    loop.exec();
    return m_open;
}

Result<std::optional<StoredBook>> Database::findByFingerprint(const QString& token)
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->findByFingerprint(token); });
}

Result<std::optional<StoredBook>> Database::findByPath(const QString& path)
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->findByPath(path); });
}

Result<std::optional<StoredBook>> Database::loadLastSession()
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->loadLastSession(); });
}

Result<std::vector<ShelfEntry>> Database::listShelf()
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->listShelf(); });
}

Result<void> Database::deleteBook(qint64 id, const QString& cacheDir)
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->deleteBook(id, cacheDir); });
}

Result<void> Database::retargetBook(qint64 id, const QString& newPath)
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->retargetBook(id, newPath); });
}

Result<qint64> Database::upsertBook(const Book& book)
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->upsertBook(book); });
}

Result<void> Database::replaceChapters(qint64 bookId, const std::vector<ChapterIndex>& chapters)
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->replaceChapters(bookId, chapters); });
}

Result<void> Database::saveProgress(const ReadingProgress& progress)
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->saveProgress(progress); });
}

Result<QString> Database::setting(const QString& key)
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->setting(key); });
}

Result<void> Database::setSettings(const std::vector<std::pair<QString, QString>>& items)
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->setSettings(items); });
}

bool Database::saveProgressAsync(const ReadingProgress& progress)
{
    DatabaseWorker* worker = m_worker;
    return QMetaObject::invokeMethod(
        worker,
        [worker, progress] { worker->saveProgress(progress); },
        Qt::QueuedConnection);
}

bool Database::setSettingsAsync(const std::vector<std::pair<QString, QString>>& items)
{
    DatabaseWorker* worker = m_worker;
    return QMetaObject::invokeMethod(
        worker,
        [worker, items] { worker->setSettings(items); },
        Qt::QueuedConnection);
}

Result<qint64> Database::addBookmark(const Bookmark& bookmark)
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->addBookmark(bookmark); });
}

Result<void> Database::deleteBookmark(qint64 id)
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->deleteBookmark(id); });
}

Result<std::vector<Bookmark>> Database::bookmarks(qint64 bookId)
{
    return runOnWorker(m_worker, &m_thread, [&] { return m_worker->bookmarks(bookId); });
}

bool Database::addBookmarkAsync(const Bookmark& bookmark)
{
    DatabaseWorker* worker = m_worker;
    return QMetaObject::invokeMethod(
        worker,
        [worker, bookmark] { worker->addBookmark(bookmark); },
        Qt::QueuedConnection);
}

bool Database::deleteBookmarkAsync(qint64 id)
{
    DatabaseWorker* worker = m_worker;
    return QMetaObject::invokeMethod(
        worker,
        [worker, id] { worker->deleteBookmark(id); },
        Qt::QueuedConnection);
}

void Database::requestSettings(QObject* context,
                               const std::vector<QString>& keys,
                               const std::function<void(const std::vector<std::pair<QString, QString>>&)>& callback)
{
    if (context == nullptr) {
        return;
    }
    DatabaseWorker* worker = m_worker;
    QMetaObject::invokeMethod(
        worker,
        [worker, context, keys, callback] {
            std::vector<std::pair<QString, QString>> items;
            items.reserve(keys.size());
            for (const QString& key : keys) {
                auto value = worker->setting(key);
                items.emplace_back(key, value ? *value : QString());
            }
            QMetaObject::invokeMethod(
                context,
                [callback, items] { callback(items); },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void Database::requestBookmarks(QObject* context,
                                qint64 bookId,
                                const std::function<void(const std::vector<Bookmark>&)>& callback)
{
    if (context == nullptr) {
        return;
    }
    DatabaseWorker* worker = m_worker;
    QMetaObject::invokeMethod(
        worker,
        [worker, context, bookId, callback] {
            auto rows = worker->bookmarks(bookId);
            const std::vector<Bookmark> marks = rows ? *rows : std::vector<Bookmark>{};
            QMetaObject::invokeMethod(
                context,
                [callback, marks] { callback(marks); },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

}  // namespace moyu

#include "Database.moc"
