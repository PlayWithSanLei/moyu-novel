#include "core/ChapterDetector.h"
#include "core/Database.h"
#include "parser/TxtParser.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include <atomic>
#include <iostream>

namespace {

struct Sample {
    const char* label = "";
    QString path;
};

void printTimings(const char* label, const QFileInfo& info, const moyu::OpenTimings& timings)
{
    std::cout << label << "  size=" << info.size() << " bytes"
              << "  cacheHit=" << (timings.cacheHit ? "yes" : "no") << "  total=" << timings.totalMs << " ms"
              << "  fingerprint=" << timings.fingerprintMs << " ms"
              << "  prepare=" << timings.prepareMs << " ms"
              << "  scan=" << timings.scanMs << " ms"
              << "  chapter=" << timings.chapterMs << " ms\n";
}

bool openOnce(const QString& path,
              const QString& cacheDir,
              moyu::Database& database,
              quint32 chapter,
              quint32 offset,
              moyu::OpenTimings* timings)
{
    std::atomic<bool> cancel{false};
    auto opened = moyu::openTxtFile(path, cacheDir, &database, chapter, offset, cancel, {}, timings);
    if (!opened) {
        std::cerr << opened.error().message.toStdString() << "\n";
        return false;
    }
    if (opened->parsed.chapters.size() > 1) {
        QElapsedTimer jump;
        jump.start();
        auto content = moyu::readMappedChapter(opened->parsed.bodyPath,
                                                opened->parsed.bodyOffset,
                                                opened->parsed.chapters[1]);
        const double ms = static_cast<double>(jump.nsecsElapsed()) / 1e6;
        if (!content) {
            std::cerr << content.error().message.toStdString() << "\n";
            return false;
        }
        std::cout << "  chapterJump=" << ms << " ms  chapters=" << opened->parsed.chapters.size() << "\n";
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QString dataDir = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                     : QDir::current().filePath(QStringLiteral("bench/data"));
    const Sample required[] = {
        {"gb18030", dataDir + QStringLiteral("/gb18030_15mb.txt")},
        {"utf8", dataDir + QStringLiteral("/utf8_100mb.txt")},
        {"gbk-bad-byte", dataDir + QStringLiteral("/gbk_bad_byte.txt")},
        {"cr-only", dataDir + QStringLiteral("/cr_only.txt")},
        {"long-chapter", dataDir + QStringLiteral("/long_chapter.txt")},
        {"ascii-prefix-gbk", dataDir + QStringLiteral("/ascii_prefix_gbk.txt")},
    };
    for (const Sample& sample : required) {
        if (!QFileInfo::exists(sample.path)) {
            std::cerr << "缺少测试书。请先运行:\n  python3 tools/gen_test_book.py\n";
            return 2;
        }
    }

    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::cerr << "无法创建临时目录\n";
        return 1;
    }
    moyu::Database database(dir.filePath(QStringLiteral("bench.db")));
    if (!database.startAndWait()) {
        std::cerr << database.errorString().toStdString() << "\n";
        return 1;
    }
    const QString cacheDir = dir.filePath(QStringLiteral("cache"));
    QDir().mkpath(cacheDir);

    for (const Sample& sample : required) {
        moyu::OpenTimings first;
        if (!openOnce(sample.path, cacheDir, database, 0, 0, &first)) {
            return 1;
        }
        printTimings(sample.label, QFileInfo(sample.path), first);

        moyu::OpenTimings second;
        if (!openOnce(sample.path, cacheDir, database, 0, 0, &second)) {
            return 1;
        }
        printTimings((QString(sample.label) + QStringLiteral("-reopen")).toStdString().c_str(),
                     QFileInfo(sample.path),
                     second);
    }

    // 这里的耗时不含进程启动。常驻内存含映射页，不是「扣除 mmap」的指标，所以不报 RSS。
    std::cout << "timings exclude process launch; RSS is not reported because mapped pages are included\n";
    return 0;
}
