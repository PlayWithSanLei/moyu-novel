#pragma once

#include "core/Database.h"
#include "parser/BookParser.h"

#include <QElapsedTimer>

#include <functional>

namespace moyu {

class TxtParser : public BookParser {
public:
    // cacheDir 是转码缓存目录。parseIndex 不读数据库，打开同一本书的缓存判断在 openTxtFile。
    explicit TxtParser(QString cacheDir);

    Result<ParsedBook> parseIndex(const QString& path, const std::atomic<bool>& cancel) override;
    Result<ChapterContent> readChapter(const ParsedBook& book, quint32 index) override;

private:
    QString m_cacheDir;
};

struct OpenTimings {
    double fingerprintMs = 0;
    double prepareMs = 0;
    double scanMs = 0;
    double chapterMs = 0;
    double totalMs = 0;
    bool cacheHit = false;
};

struct TxtDocument {
    ParsedBook parsed;
    ChapterContent chapter;
    quint32 chapterIndex = 0;
    quint32 byteOffset = 0;
    bool cacheHit = false;
};

// 扫描还在进行时，先把锚点附近大约 8000 个码点交出来，界面可以先画字。
using PreviewHandler = std::function<void(ChapterContent content, quint64 bodyByteStart, quint64 totalBytes)>;

Result<ChapterContent> readMappedChapter(const QString& bodyPath,
                                          quint64 bodyOffset,
                                          const ChapterIndex& chapter);

// 打开任意格式的最后一步：按提示定位章节，并把偏移夹进这一章。
Result<TxtDocument> assembleDocument(ParsedBook parsed,
                                     quint32 chapterHint,
                                     quint32 byteHint,
                                     bool cacheHit,
                                     OpenTimings* timings,
                                     QElapsedTimer& total);

// database 可以为空，这时每次都重新扫描。命中缓存时不发预览，避免正文闪一下。
Result<TxtDocument> openTxtFile(const QString& path,
                                 const QString& cacheDir,
                                 Database* database,
                                 quint32 chapterHint,
                                 quint32 byteHint,
                                 const std::atomic<bool>& cancel,
                                 const PreviewHandler& preview,
                                 OpenTimings* timings,
                                 const QString& chapterPattern = {});

}  // namespace moyu
