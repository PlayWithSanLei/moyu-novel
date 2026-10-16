#include "app/BookSession.h"

#include "core/ChapterDetector.h"
#include "core/Database.h"
#include "core/Fingerprint.h"
#include "core/MappedFile.h"
#include "core/TextSearch.h"
#include "core/Utf8.h"
#include "parser/OpenBook.h"

#include <QThreadPool>

#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <deque>
#include <limits>
#include <utility>

namespace moyu {
namespace {

constexpr int kSaveDelayMs = 1000;

QString previewAt(const char* data, quint64 size, quint64 offset, int codePoints)
{
    if (data == nullptr || offset >= size || codePoints <= 0) {
        return {};
    }
    quint64 cursor = offset;
    QString text;
    int count = 0;
    while (cursor < size && count < codePoints) {
        const int consumed = utf8SequenceLength(data + cursor, data + size);
        if (consumed <= 0) {
            break;
        }
        text += QString::fromUtf8(data + cursor, consumed);
        cursor += static_cast<quint64>(consumed);
        ++count;
    }
    return text;
}

SearchHitInfo hitInfo(const std::vector<ChapterIndex>& chapters, const char* data, quint64 size, quint64 offset)
{
    SearchHitInfo info;
    if (chapters.empty()) {
        info.preview = previewAt(data, size, offset, 50);
        return info;
    }
    std::size_t at = 0;
    for (std::size_t i = 0; i < chapters.size(); ++i) {
        if (chapters[i].byteStart <= offset) {
            at = i;
        } else {
            break;
        }
    }
    const ChapterIndex& chapter = chapters[at];
    info.chapterIndex = chapter.index;
    const quint64 local = offset >= chapter.byteStart ? offset - chapter.byteStart : 0;
    info.byteOffset = local > std::numeric_limits<quint32>::max() ? std::numeric_limits<quint32>::max()
                                                                   : static_cast<quint32>(local);
    info.preview = previewAt(data, size, offset, 50);
    return info;
}

}  // namespace

struct BookSession::Job {
    enum class Kind { Open, OpenLast, Import, Regenerate, Jump, Preload, Search };

    Kind kind = Kind::Open;
    quint64 generation = 0;
    quint64 viewToken = 0;
    QString path;
    QStringList paths;
    QString chapterPattern;
    qint64 bookId = 0;
    ParsedBook parsed;
    bool resume = false;
    quint32 chapter = 0;
    quint32 offset = 0;
    bool pinToEnd = false;
    QString bodyPath;
    quint64 bodyOffset = 0;
    ChapterIndex chapterMeta;
    std::vector<ChapterIndex> chapters;
    QString query;
    quint64 totalBytes = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
};

struct BookSession::Loader : public QObject {
    Loader(BookSession* session, Database* database, QString cacheDir, std::atomic<quint64>* generation)
        : m_session(session)
        , m_database(database)
        , m_cacheDir(std::move(cacheDir))
        , m_generation(generation)
    {
    }

    void enqueue(Job job)
    {
        if (m_shuttingDown) {
            return;
        }
        if (job.kind == Job::Kind::Open || job.kind == Job::Kind::OpenLast || job.kind == Job::Kind::Import) {
            m_queue.clear();
        } else if (job.kind == Job::Kind::Jump) {
            std::deque<Job> kept;
            for (Job& queued : m_queue) {
                if (queued.kind == Job::Kind::Open || queued.kind == Job::Kind::OpenLast ||
                    queued.kind == Job::Kind::Import || queued.kind == Job::Kind::Regenerate ||
                    queued.kind == Job::Kind::Search) {
                    kept.push_back(std::move(queued));
                }
            }
            m_queue.swap(kept);
        }
        m_queue.push_back(std::move(job));
        // 排到下一轮再跑。正在执行的任务返回后才能看到新队列，取消和新任务不会叠在同一次调用里。
        QMetaObject::invokeMethod(this, [this] { kick(); }, Qt::QueuedConnection);
    }

    void prepareShutdown()
    {
        m_shuttingDown = true;
        m_queue.clear();
        moveToThread(nullptr);
    }

    void kick()
    {
        if (m_running || m_queue.empty() || m_shuttingDown) {
            return;
        }
        m_running = true;
        Job job = std::move(m_queue.front());
        m_queue.pop_front();
        // 不把取消标记改回 false。新的打开已经换了一块自己的标记，旧任务只能看见自己的。
        if (job.generation == m_generation->load() && !(job.cancel && job.cancel->load())) {
            run(job);
        }
        m_running = false;
        if (!m_queue.empty() && !m_shuttingDown) {
            QMetaObject::invokeMethod(this, [this] { kick(); }, Qt::QueuedConnection);
        }
    }

    void run(const Job& job)
    {
        switch (job.kind) {
        case Job::Kind::Open:
            runOpen(job);
            break;
        case Job::Kind::OpenLast:
            runOpenLast(job);
            break;
        case Job::Kind::Import:
            runImport(job);
            break;
        case Job::Kind::Regenerate:
            runRegenerate(job);
            break;
        case Job::Kind::Jump:
        case Job::Kind::Preload:
            runRead(job);
            break;
        case Job::Kind::Search:
            runSearch(job);
            break;
        }
    }

    void runOpen(const Job& job)
    {
        quint32 chapter = 0;
        quint32 offset = 0;
        if (job.resume && m_database != nullptr && m_database->isOpen()) {
            auto fingerprint = fingerprintOf(job.path);
            if (fingerprint) {
                auto cached = m_database->findByFingerprint(fingerprint->token());
                const StoredBook* stored = (cached && cached->has_value()) ? &(**cached) : nullptr;
                Result<std::optional<StoredBook>> byPath{std::optional<StoredBook>{}};
                if (stored == nullptr && cached) {
                    // 指纹对不上时按路径再找。连载更新后文件变了，进度还在原来那一行。
                    byPath = m_database->findByPath(job.path);
                    if (byPath && byPath->has_value()) {
                        stored = &(**byPath);
                    }
                }
                if (stored != nullptr && stored->progress.has_value()) {
                    chapter = stored->progress->chapterIndex;
                    offset = stored->progress->byteOffsetInChapter;
                }
            }
        }
        if (job.generation != m_generation->load() || (job.cancel && job.cancel->load())) {
            return;
        }
        openAt(job, job.path, chapter, offset);
    }

    void runOpenLast(const Job& job)
    {
        if (m_database == nullptr || !m_database->isOpen()) {
            return;
        }
        auto last = m_database->loadLastSession();
        if (!last) {
            postFailure(job, last.error().message, false);
            return;
        }
        if (!last->has_value()) {
            return;
        }
        const StoredBook& stored = **last;
        if (!QFileInfo::exists(stored.book.filePath)) {
            const quint64 generation = job.generation;
            const qint64 bookId = stored.book.id;
            const QString title = stored.book.title;
            const QString path = stored.book.filePath;
            QMetaObject::invokeMethod(
                m_session,
                [this, generation, bookId, title, path] {
                    if (generation != m_generation->load()) {
                        return;
                    }
                    m_session->m_suspendAnchor = false;
                    m_session->m_previewPosted = false;
                    Q_EMIT m_session->fileMissing(bookId, title, path);
                },
                Qt::QueuedConnection);
            return;
        }
        quint32 chapter = 0;
        quint32 offset = 0;
        if (stored.progress.has_value()) {
            chapter = stored.progress->chapterIndex;
            offset = stored.progress->byteOffsetInChapter;
        }
        openAt(job, stored.book.filePath, chapter, offset);
    }

    void resumeAt(const Job& job, const QString& path, quint32* chapter, quint32* offset)
    {
        *chapter = 0;
        *offset = 0;
        if (!job.resume || m_database == nullptr || !m_database->isOpen()) {
            return;
        }
        auto fingerprint = fingerprintOf(path);
        if (!fingerprint) {
            return;
        }
        auto cached = m_database->findByFingerprint(fingerprint->token());
        const StoredBook* stored = (cached && cached->has_value()) ? &(**cached) : nullptr;
        if (stored == nullptr && cached) {
            auto byPath = m_database->findByPath(path);
            if (byPath && byPath->has_value()) {
                stored = &(**byPath);
            }
        }
        if (stored != nullptr && stored->progress.has_value()) {
            *chapter = stored->progress->chapterIndex;
            *offset = stored->progress->byteOffsetInChapter;
        }
    }

    void publish(const Job& job, TxtDocument document)
    {
        const quint64 generation = job.generation;
        QMetaObject::invokeMethod(
            m_session,
            [this, generation, document = std::move(document)]() mutable {
                if (generation != m_generation->load()) {
                    return;
                }
                m_session->deliverBook(document);
            },
            Qt::QueuedConnection);
    }

    Result<TxtDocument> loadOne(const Job& job, const QString& path, quint32 chapter, quint32 offset)
    {
        const quint64 generation = job.generation;
        auto preview = [this, generation](ChapterContent content, quint64 bodyByteStart, quint64 totalBytes) {
            ChapterContent delivered = std::move(content);
            QMetaObject::invokeMethod(
                m_session,
                [this, generation, delivered = std::move(delivered), bodyByteStart, totalBytes]() mutable {
                    if (generation != m_generation->load()) {
                        return;
                    }
                    m_session->m_previewPosted = true;
                    Q_EMIT m_session->previewReady(std::move(delivered), bodyByteStart, totalBytes);
                },
                Qt::QueuedConnection);
        };
        static std::atomic<bool> keepGoing{false};
        const std::atomic<bool>& cancel = job.cancel ? *job.cancel : keepGoing;
        return openBookFile(path, m_cacheDir, m_database, chapter, offset, cancel, preview, nullptr, job.chapterPattern);
    }

    void runImport(const Job& job)
    {
        std::optional<TxtDocument> last;
        QString failure;
        for (const QString& path : job.paths) {
            if (job.generation != m_generation->load() || (job.cancel && job.cancel->load())) {
                return;
            }
            quint32 chapter = 0;
            quint32 offset = 0;
            resumeAt(job, path, &chapter, &offset);
            auto opened = loadOne(job, path, chapter, offset);
            if (job.generation != m_generation->load()) {
                return;
            }
            if (!opened) {
                if (!opened.error().cancelled) {
                    failure = opened.error().message;
                }
                continue;
            }
            last = std::move(*opened);
        }
        if (job.generation != m_generation->load()) {
            return;
        }
        if (!last.has_value()) {
            postFailure(job, failure.isEmpty() ? QStringLiteral("不支持的文件格式") : failure, true);
            return;
        }
        publish(job, std::move(*last));
        if (!failure.isEmpty()) {
            postFailure(job, failure, false);
        }
    }

    void runRegenerate(const Job& job)
    {
        if (job.bookId <= 0 || job.parsed.bodyPath.isEmpty() || job.parsed.book.format != BookFormat::Txt) {
            postFailure(job, QStringLiteral("只有 TXT 会按自定义正则重新分章"), false);
            return;
        }
        auto mapped = MappedFile::open(job.parsed.bodyPath);
        if (!mapped) {
            postFailure(job, mapped.error().message, false);
            return;
        }
        if (job.parsed.bodyOffset > mapped->size()) {
            postFailure(job, QStringLiteral("缓存文件损坏"), false);
            return;
        }
        const quint64 size = mapped->size() - job.parsed.bodyOffset;
        if (size == 0 || size != job.parsed.book.totalBytes) {
            postFailure(job, QStringLiteral("缓存文件损坏"), false);
            return;
        }
        const std::atomic<bool>* cancel = job.cancel.get();
        auto chapters = detectChapters(mapped->data() + job.parsed.bodyOffset, size, cancel, job.chapterPattern);
        if (job.generation != m_generation->load() || (job.cancel && job.cancel->load())) {
            return;
        }
        if (!chapters) {
            postFailure(job, chapters.error().message, false);
            return;
        }
        if (m_database == nullptr || !m_database->isOpen()) {
            postFailure(job, QStringLiteral("无法打开数据库"), false);
            return;
        }
        auto replaced = m_database->replaceChapters(job.bookId, *chapters);
        if (!replaced) {
            postFailure(job, replaced.error().message, false);
            return;
        }
        ParsedBook parsed = job.parsed;
        parsed.chapters = std::move(*chapters);
        QElapsedTimer total;
        total.start();
        auto document = assembleDocument(std::move(parsed), job.chapter, job.offset, true, nullptr, total);
        if (job.generation != m_generation->load()) {
            return;
        }
        if (!document) {
            postFailure(job, document.error().message, false);
            return;
        }
        publish(job, std::move(*document));
    }

    void openAt(const Job& job, const QString& path, quint32 chapter, quint32 offset)
    {
        const quint64 generation = job.generation;
        auto preview = [this, generation](ChapterContent content, quint64 bodyByteStart, quint64 totalBytes) {
            ChapterContent delivered = std::move(content);
            QMetaObject::invokeMethod(
                m_session,
                [this, generation, delivered = std::move(delivered), bodyByteStart, totalBytes]() mutable {
                    if (generation != m_generation->load()) {
                        return;
                    }
                    m_session->m_previewPosted = true;
                    Q_EMIT m_session->previewReady(std::move(delivered), bodyByteStart, totalBytes);
                },
                Qt::QueuedConnection);
        };
        static std::atomic<bool> keepGoing{false};
        const std::atomic<bool>& cancel = job.cancel ? *job.cancel : keepGoing;
        auto opened = openBookFile(path, m_cacheDir, m_database, chapter, offset, cancel, preview, nullptr, job.chapterPattern);
        if (generation != m_generation->load()) {
            return;
        }
        if (!opened) {
            if (!opened.error().cancelled) {
                postFailure(job, opened.error().message, true);
            }
            return;
        }
        TxtDocument document = std::move(*opened);
        QMetaObject::invokeMethod(
            m_session,
            [this, generation, document = std::move(document)]() mutable {
                if (generation != m_generation->load()) {
                    return;
                }
                m_session->deliverBook(document);
            },
            Qt::QueuedConnection);
    }

    void runRead(const Job& job)
    {
        if (job.cancel && job.cancel->load()) {
            return;
        }
        auto content = readMappedChapter(job.bodyPath, job.bodyOffset, job.chapterMeta);
        if (job.generation != m_generation->load() || (job.cancel && job.cancel->load())) {
            return;
        }
        if (!content) {
            const quint32 chapter = job.chapter;
            QMetaObject::invokeMethod(
                m_session,
                [this, chapter] { m_session->m_inflight.erase(chapter); },
                Qt::QueuedConnection);
            if (job.kind == Job::Kind::Jump && !content.error().cancelled) {
                postFailure(job, content.error().message, false);
            }
            return;
        }
        ChapterContent delivered = std::move(*content);
        const bool jump = job.kind == Job::Kind::Jump;
        QMetaObject::invokeMethod(
            m_session,
            [this, job, delivered = std::move(delivered), jump]() mutable {
                m_session->m_inflight.erase(job.chapter);
                if (job.generation != m_generation->load() || job.viewToken != m_session->m_viewToken.load()) {
                    return;
                }
                if (!m_session->m_book.has_value()) {
                    return;
                }
                if (jump) {
                    m_session->deliverChapter(std::move(delivered), job.chapter, job.offset, job.pinToEnd);
                    return;
                }
                m_session->deliverNeighbor(std::move(delivered), job.chapter);
            },
            Qt::QueuedConnection);
    }

    void runSearch(const Job& job)
    {
        auto mapped = MappedFile::open(job.bodyPath);
        if (!mapped || job.bodyOffset > mapped->size()) {
            const quint64 generation = job.generation;
            QMetaObject::invokeMethod(
                m_session,
                [this, generation] {
                    if (generation != m_generation->load()) {
                        return;
                    }
                    Q_EMIT m_session->searchBatch({}, true);
                },
                Qt::QueuedConnection);
            return;
        }
        auto file = std::make_shared<MappedFile>(std::move(*mapped));
        const quint64 bodyOffset = job.bodyOffset;
        const quint64 size = file->size() - bodyOffset;
        const QByteArray needle = job.query.toUtf8();
        auto chapters = std::make_shared<std::vector<ChapterIndex>>(job.chapters);
        auto cancel = job.cancel;
        const quint64 generation = job.generation;
        QPointer<BookSession> session(m_session);
        auto jobs = m_session->m_searchJobs;
        jobs->fetch_add(1);
        QThreadPool::globalInstance()->start([file, bodyOffset, size, needle, chapters, cancel, generation, session, jobs] {
            struct Leave {
                std::shared_ptr<std::atomic<int>> counter;
                ~Leave() { counter->fetch_sub(1); }
            } leave{jobs};
            std::atomic<bool> idle{false};
            const std::atomic<bool>& flag = cancel ? *cancel : idle;
            auto searched = searchUtf8(file->data() + bodyOffset,
                                        size,
                                        needle,
                                        1ULL << 18,
                                        flag,
                                        [generation, chapters, file, bodyOffset, size, session](const std::vector<SearchHit>& hits) {
                                            if (session.isNull()) {
                                                return;
                                            }
                                            std::vector<SearchHitInfo> infos;
                                            infos.reserve(hits.size());
                                            const char* body = file->data() + bodyOffset;
                                            for (const SearchHit& hit : hits) {
                                                infos.push_back(hitInfo(*chapters, body, size, hit.byteOffset));
                                            }
                                            QMetaObject::invokeMethod(
                                                session.data(),
                                                [session, generation, infos = std::move(infos)] {
                                                    if (session.isNull() || generation != session->m_generation.load()) {
                                                        return;
                                                    }
                                                    Q_EMIT session->searchBatch(infos, false);
                                                },
                                                Qt::QueuedConnection);
                                        },
                                        nullptr);
            Q_UNUSED(searched);
            if (session.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                session.data(),
                [session, generation] {
                    if (session.isNull() || generation != session->m_generation.load()) {
                        return;
                    }
                    Q_EMIT session->searchBatch({}, true);
                },
                Qt::QueuedConnection);
        });
    }

    void postFailure(const Job& job, const QString& message, bool maybeClear)
    {
        const quint64 generation = job.generation;
        QMetaObject::invokeMethod(
            m_session,
            [this, generation, message, maybeClear] {
                if (generation != m_generation->load()) {
                    return;
                }
                const bool hadPreview = m_session->m_previewPosted;
                const bool hadBook = m_session->m_book.has_value();
                bool restored = false;
                if (maybeClear && hadPreview && hadBook) {
                    restored = m_session->restoreAfterFailedOpen();
                }
                const bool clear = maybeClear && hadPreview && !restored;
                m_session->m_suspendAnchor = false;
                m_session->m_previewPosted = false;
                Q_EMIT m_session->openFailed(message, clear);
            },
            Qt::QueuedConnection);
    }

    BookSession* m_session = nullptr;
    Database* m_database = nullptr;
    QString m_cacheDir;
    std::atomic<quint64>* m_generation = nullptr;
    std::deque<Job> m_queue;
    bool m_running = false;
    bool m_shuttingDown = false;
};

BookSession::BookSession(Database* database, QString cacheDir, QObject* parent)
    : QObject(parent)
    , m_database(database)
    , m_cacheDir(std::move(cacheDir))
    , m_thread(std::make_unique<QThread>())
    , m_loader(new Loader(this, database, m_cacheDir, &m_generation))
{
    m_progressTimer = new QTimer(this);
    m_progressTimer->setSingleShot(true);
    m_progressTimer->setInterval(kSaveDelayMs);
    connect(m_progressTimer, &QTimer::timeout, this, [this] { flushProgress(); });

    m_geometryTimer = new QTimer(this);
    m_geometryTimer->setSingleShot(true);
    m_geometryTimer->setInterval(kSaveDelayMs);
    connect(m_geometryTimer, &QTimer::timeout, this, [this] { flushSettings(); });

    m_thread->setObjectName(QStringLiteral("moyu-loader"));
    m_loader->moveToThread(m_thread.get());
    m_thread->start();

    // 等加载线程的事件循环起来，析构时排队的 shutdown 才能被收到。
    QEventLoop ready;
    QMetaObject::invokeMethod(
        m_loader,
        [&ready] {
            QMetaObject::invokeMethod(&ready, [&ready] { ready.quit(); }, Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    ready.exec();
}

BookSession::~BookSession()
{
    cancelSearch();
    QElapsedTimer searchWait;
    searchWait.start();
    while (m_searchJobs->load() > 0 && searchWait.elapsed() < 2000) {
        QThread::msleep(5);
    }
    m_progressTimer->stop();
    m_geometryTimer->stop();
    m_generation.fetch_add(1);
    if (m_activeCancel) {
        m_activeCancel->store(true);
    }
    flushNow();
    QMetaObject::invokeMethod(m_loader, [loader = m_loader] { loader->prepareShutdown(); }, Qt::QueuedConnection);
    m_thread->quit();
    m_thread->wait();
    delete m_loader;
    m_loader = nullptr;
}

void BookSession::enqueue(const Job& job)
{
    QMetaObject::invokeMethod(m_loader, [loader = m_loader, job] { loader->enqueue(job); }, Qt::QueuedConnection);
}

void BookSession::openFile(const QString& path)
{
    if (!isSupportedBookPath(path)) {
        Q_EMIT openFailed(QStringLiteral("不支持的文件格式"), false);
        return;
    }
    flushProgress();
    m_suspendAnchor = true;
    m_previewPosted = false;
    const quint64 generation = m_generation.fetch_add(1) + 1;
    if (m_activeCancel) {
        m_activeCancel->store(true);
    }
    m_activeCancel = std::make_shared<std::atomic<bool>>(false);
    Job job;
    job.kind = Job::Kind::Open;
    job.generation = generation;
    job.path = path;
    job.chapterPattern = m_chapterPattern;
    job.resume = true;
    job.cancel = m_activeCancel;
    enqueue(job);
}

void BookSession::openFiles(const QStringList& paths)
{
    QStringList accepted;
    for (const QString& path : paths) {
        if (isSupportedBookPath(path)) {
            accepted.push_back(path);
        }
    }
    if (accepted.isEmpty()) {
        Q_EMIT openFailed(QStringLiteral("不支持的文件格式"), false);
        return;
    }
    if (accepted.size() == 1) {
        openFile(accepted.front());
        return;
    }
    flushProgress();
    m_suspendAnchor = true;
    m_previewPosted = false;
    const quint64 generation = m_generation.fetch_add(1) + 1;
    if (m_activeCancel) {
        m_activeCancel->store(true);
    }
    m_activeCancel = std::make_shared<std::atomic<bool>>(false);
    Job job;
    job.kind = Job::Kind::Import;
    job.generation = generation;
    job.paths = accepted;
    job.chapterPattern = m_chapterPattern;
    job.resume = true;
    job.cancel = m_activeCancel;
    enqueue(job);
}

void BookSession::openLast()
{
    flushProgress();
    m_suspendAnchor = true;
    m_previewPosted = false;
    const quint64 generation = m_generation.fetch_add(1) + 1;
    if (m_activeCancel) {
        m_activeCancel->store(true);
    }
    m_activeCancel = std::make_shared<std::atomic<bool>>(false);
    Job job;
    job.kind = Job::Kind::OpenLast;
    job.generation = generation;
    job.chapterPattern = m_chapterPattern;
    job.cancel = m_activeCancel;
    enqueue(job);
}

void BookSession::setChapterPattern(const QString& pattern)
{
    m_chapterPattern = pattern;
}

void BookSession::regenerateChapters()
{
    if (!m_book.has_value() || m_book->book.id <= 0) {
        Q_EMIT openFailed(QStringLiteral("还没有打开书"), false);
        return;
    }
    if (m_book->book.format != BookFormat::Txt) {
        Q_EMIT openFailed(QStringLiteral("只有 TXT 会按自定义正则重新分章"), false);
        return;
    }
    flushProgress();
    const quint64 generation = m_generation.fetch_add(1) + 1;
    if (m_activeCancel) {
        m_activeCancel->store(true);
    }
    m_activeCancel = std::make_shared<std::atomic<bool>>(false);
    Job job;
    job.kind = Job::Kind::Regenerate;
    job.generation = generation;
    job.chapterPattern = m_chapterPattern;
    job.bookId = m_book->book.id;
    job.parsed = *m_book;
    job.chapter = m_pendingChapter;
    job.offset = m_pendingOffset;
    job.cancel = m_activeCancel;
    enqueue(job);
}

void BookSession::removeFromShelf(qint64 id)
{
    if (m_database == nullptr || !m_database->isOpen()) {
        Q_EMIT openFailed(QStringLiteral("无法打开数据库"), false);
        return;
    }
    const bool current = m_book.has_value() && m_book->book.id == id;
    auto removed = m_database->deleteBook(id, m_cacheDir);
    if (!removed) {
        Q_EMIT openFailed(removed.error().message, false);
        return;
    }
    if (current) {
        m_book.reset();
        m_cache.clear();
        m_inflight.clear();
        m_progressDirty = false;
        m_suspendAnchor = false;
        Q_EMIT bookCleared();
    }
    Q_EMIT shelfChanged();
}

void BookSession::goToChapter(quint32 index, bool pinToEnd)
{
    if (m_suspendAnchor || !m_book.has_value() || index >= m_book->chapters.size()) {
        return;
    }
    // 旧的预读还在队列里时，完成回调会先清 inflight。这里先清掉，避免把新的预读挡住。
    m_inflight.clear();
    m_viewToken.fetch_add(1);
    const quint64 token = m_viewToken.load();
    const auto found = m_cache.find(index);
    if (found != m_cache.end()) {
        deliverChapter(found->second, index, 0, pinToEnd);
        return;
    }
    Job job;
    job.kind = Job::Kind::Jump;
    job.generation = m_generation.load();
    job.viewToken = token;
    job.chapter = index;
    job.pinToEnd = pinToEnd;
    job.bodyPath = m_book->bodyPath;
    job.bodyOffset = m_book->bodyOffset;
    job.chapterMeta = m_book->chapters[index];
    job.totalBytes = m_book->book.totalBytes;
    job.cancel = m_activeCancel;
    enqueue(job);
}

void BookSession::goToAnchor(quint32 index, quint32 offset)
{
    if (m_suspendAnchor || !m_book.has_value() || index >= m_book->chapters.size()) {
        return;
    }
    m_inflight.clear();
    m_viewToken.fetch_add(1);
    const quint64 token = m_viewToken.load();
    const auto found = m_cache.find(index);
    if (found != m_cache.end()) {
        deliverChapter(found->second, index, offset, false);
        return;
    }
    Job job;
    job.kind = Job::Kind::Jump;
    job.generation = m_generation.load();
    job.viewToken = token;
    job.chapter = index;
    job.offset = offset;
    job.bodyPath = m_book->bodyPath;
    job.bodyOffset = m_book->bodyOffset;
    job.chapterMeta = m_book->chapters[index];
    job.totalBytes = m_book->book.totalBytes;
    job.cancel = m_activeCancel;
    enqueue(job);
}

void BookSession::requestNeighbor(quint32 index)
{
    if (m_suspendAnchor || !m_book.has_value() || index >= m_book->chapters.size()) {
        return;
    }
    if (m_inflight.contains(index)) {
        return;
    }
    const auto found = m_cache.find(index);
    if (found != m_cache.end()) {
        deliverNeighbor(found->second, index);
        return;
    }
    m_inflight.insert(index);
    Job job;
    job.kind = Job::Kind::Preload;
    job.generation = m_generation.load();
    job.viewToken = m_viewToken.load();
    job.chapter = index;
    job.bodyPath = m_book->bodyPath;
    job.bodyOffset = m_book->bodyOffset;
    job.chapterMeta = m_book->chapters[index];
    job.totalBytes = m_book->book.totalBytes;
    job.cancel = m_activeCancel;
    enqueue(job);
}

void BookSession::search(const QString& query)
{
    if (m_searchCancel) {
        m_searchCancel->store(true);
    }
    if (!m_book.has_value()) {
        Q_EMIT searchBatch({}, true);
        return;
    }
    const QString text = query.trimmed();
    if (text.isEmpty()) {
        m_searchCancel.reset();
        Q_EMIT searchBatch({}, true);
        return;
    }
    m_searchCancel = std::make_shared<std::atomic<bool>>(false);
    Job job;
    job.kind = Job::Kind::Search;
    job.generation = m_generation.load();
    job.bodyPath = m_book->bodyPath;
    job.bodyOffset = m_book->bodyOffset;
    job.chapters = m_book->chapters;
    job.query = text;
    job.totalBytes = m_book->book.totalBytes;
    job.cancel = m_searchCancel;
    enqueue(job);
}

void BookSession::cancelSearch()
{
    if (m_searchCancel) {
        m_searchCancel->store(true);
    }
}

void BookSession::addBookmark(quint32 chapter, quint32 offset, const QString& preview)
{
    if (!m_book.has_value() || m_book->book.id <= 0 || m_database == nullptr || !m_database->isOpen()) {
        return;
    }
    Bookmark mark;
    mark.bookId = m_book->book.id;
    mark.chapterIndex = chapter;
    mark.byteOffsetInChapter = offset;
    mark.preview = preview.left(50);
    mark.createdAt = QDateTime::currentMSecsSinceEpoch();
    m_database->addBookmarkAsync(mark);
}

void BookSession::removeBookmark(qint64 id)
{
    if (m_database == nullptr || !m_database->isOpen()) {
        return;
    }
    m_database->deleteBookmarkAsync(id);
}

void BookSession::requestBookmarks()
{
    if (!m_book.has_value() || m_book->book.id <= 0 || m_database == nullptr || !m_database->isOpen()) {
        Q_EMIT bookmarksReady({});
        return;
    }
    const qint64 id = m_book->book.id;
    m_database->requestBookmarks(this, id, [this, id](const std::vector<Bookmark>& rows) {
        if (!m_book.has_value() || m_book->book.id != id) {
            return;
        }
        Q_EMIT bookmarksReady(rows);
    });
}

qint64 BookSession::bookId() const
{
    return m_book.has_value() ? m_book->book.id : 0;
}

void BookSession::noteAnchor(quint32 chapterIndex, quint32 byteOffset)
{
    if (m_suspendAnchor || !m_book.has_value()) {
        return;
    }
    if (chapterIndex < m_book->chapters.size() && chapterIndex != m_currentIndex) {
        m_currentIndex = chapterIndex;
        preloadAround(chapterIndex);
    }
    if (chapterIndex == m_savedChapter && byteOffset == m_savedOffset && !m_progressDirty) {
        return;
    }
    m_pendingChapter = chapterIndex;
    m_pendingOffset = byteOffset;
    m_progressDirty = true;
    if (chapterIndex != m_savedChapter) {
        m_progressTimer->stop();
        flushProgress();
        return;
    }
    m_progressTimer->start();
}

void BookSession::noteGeometry(QRect geometry)
{
    if (geometry == m_geometry && !m_geometryDirty) {
        return;
    }
    m_geometry = geometry;
    m_geometryDirty = true;
    m_geometryTimer->start();
}

void BookSession::setAlwaysOnTop(bool on)
{
    if (m_onTop == on && !m_onTopDirty) {
        return;
    }
    m_onTop = on;
    m_onTopDirty = true;
    flushSettings();
}

void BookSession::flushNow()
{
    m_progressTimer->stop();
    m_geometryTimer->stop();
    flushProgress();
    flushSettings();
}

void BookSession::requestPlacement()
{
    if (m_database == nullptr || !m_database->isOpen()) {
        Q_EMIT placementReady(WindowPlacement{});
        return;
    }
    const std::vector<QString> keys = {
        QStringLiteral("window/x"),
        QStringLiteral("window/y"),
        QStringLiteral("window/width"),
        QStringLiteral("window/height"),
        QStringLiteral("window/onTop"),
    };
    m_database->requestSettings(this, keys, [this](const std::vector<std::pair<QString, QString>>& items) {
        QString x;
        QString y;
        QString width;
        QString height;
        QString top;
        for (const auto& item : items) {
            if (item.first == QLatin1String("window/x")) {
                x = item.second;
            } else if (item.first == QLatin1String("window/y")) {
                y = item.second;
            } else if (item.first == QLatin1String("window/width")) {
                width = item.second;
            } else if (item.first == QLatin1String("window/height")) {
                height = item.second;
            } else if (item.first == QLatin1String("window/onTop")) {
                top = item.second;
            }
        }
        WindowPlacement place;
        bool okX = false;
        bool okY = false;
        bool okW = false;
        bool okH = false;
        const int xi = x.toInt(&okX);
        const int yi = y.toInt(&okY);
        const int wi = width.toInt(&okW);
        const int hi = height.toInt(&okH);
        if (okX && okY && okW && okH && wi >= 240 && hi >= 160) {
            place.geometry = QRect(xi, yi, wi, hi);
            place.hasGeometry = true;
        }
        if (top == QLatin1String("1") || top == QLatin1String("0")) {
            place.hasOnTop = true;
            place.onTop = top == QLatin1String("1");
        }
        Q_EMIT placementReady(place);
    });
}

void BookSession::deliverBook(const TxtDocument& document)
{
    m_suspendAnchor = false;
    m_previewPosted = false;
    m_book = document.parsed;
    m_currentIndex = document.chapterIndex;
    m_savedChapter = document.chapterIndex;
    m_savedOffset = document.byteOffset;
    m_pendingChapter = document.chapterIndex;
    m_pendingOffset = document.byteOffset;
    m_progressDirty = false;
    m_cache.clear();
    m_inflight.clear();
    remember(document.chapterIndex, document.chapter);
    m_viewToken.fetch_add(1);
    Q_EMIT bookReady(document);
    preloadAround(document.chapterIndex);
}

void BookSession::deliverChapter(ChapterContent content, quint32 index, quint32 byteOffset, bool pinToEnd)
{
    if (!m_book.has_value() || index >= m_book->chapters.size()) {
        return;
    }
    m_currentIndex = index;
    remember(index, content);
    const ChapterIndex& chapter = m_book->chapters[index];
    Q_EMIT chapterReady(std::move(content), chapter.byteStart, m_book->book.totalBytes, byteOffset, pinToEnd);
    preloadAround(index);
}

void BookSession::deliverNeighbor(ChapterContent content, quint32 index)
{
    if (!m_book.has_value() || index >= m_book->chapters.size()) {
        return;
    }
    m_inflight.erase(index);
    remember(index, content);
    const ChapterIndex& chapter = m_book->chapters[index];
    Q_EMIT neighborReady(std::move(content), chapter.byteStart, m_book->book.totalBytes);
}

void BookSession::preloadAround(quint32 index)
{
    // 走 requestNeighbor：缓存里已有的邻居会直接拼上，正在读的不会再排一次。
    if (index > 0) {
        requestNeighbor(index - 1);
    }
    requestNeighbor(index + 1);
}

void BookSession::remember(quint32 index, const ChapterContent& content)
{
    m_cache[index] = content;
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        const quint32 other = it->first;
        const quint32 distance = other > index ? other - index : index - other;
        if (distance > 1) {
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void BookSession::flushProgress()
{
    if (!m_progressDirty || !m_book.has_value() || m_book->book.id <= 0 || m_database == nullptr || !m_database->isOpen()) {
        m_progressDirty = false;
        return;
    }
    ReadingProgress progress;
    progress.bookId = m_book->book.id;
    progress.chapterIndex = m_pendingChapter;
    progress.byteOffsetInChapter = m_pendingOffset;
    progress.updatedAt = QDateTime::currentMSecsSinceEpoch();
    // 排队成功就清掉脏标记。退出时这条事件排在数据库线程的 shutdown 前面，还是会写完。
    if (m_database->saveProgressAsync(progress)) {
        m_savedChapter = m_pendingChapter;
        m_savedOffset = m_pendingOffset;
        m_progressDirty = false;
    }
}

void BookSession::flushSettings()
{
    if (m_database == nullptr || !m_database->isOpen() || (!m_geometryDirty && !m_onTopDirty)) {
        return;
    }
    std::vector<std::pair<QString, QString>> items;
    if (m_geometryDirty) {
        items.emplace_back(QStringLiteral("window/x"), QString::number(m_geometry.x()));
        items.emplace_back(QStringLiteral("window/y"), QString::number(m_geometry.y()));
        items.emplace_back(QStringLiteral("window/width"), QString::number(m_geometry.width()));
        items.emplace_back(QStringLiteral("window/height"), QString::number(m_geometry.height()));
    }
    if (m_onTopDirty) {
        items.emplace_back(QStringLiteral("window/onTop"), m_onTop ? QStringLiteral("1") : QStringLiteral("0"));
    }
    if (m_database->setSettingsAsync(items)) {
        m_geometryDirty = false;
        m_onTopDirty = false;
    }
}

bool BookSession::restoreAfterFailedOpen()
{
    m_suspendAnchor = false;
    m_previewPosted = false;
    const auto found = m_cache.find(m_currentIndex);
    if (found == m_cache.end() || !m_book.has_value() || m_currentIndex >= m_book->chapters.size()) {
        m_book.reset();
        m_cache.clear();
        return false;
    }
    const ChapterIndex& chapter = m_book->chapters[m_currentIndex];
    Q_EMIT chapterReady(found->second, chapter.byteStart, m_book->book.totalBytes, m_savedOffset, false);
    return true;
}

}  // namespace moyu
