#include "core/ChapterDetector.h"
#include "core/Database.h"
#include "core/Encoding.h"
#include "core/Fingerprint.h"
#include "core/MappedFile.h"
#include "core/Paths.h"
#include "core/Utf8.h"
#include "core/TextSearch.h"
#include "parser/Markup.h"
#include "parser/OpenBook.h"
#include "parser/TxtParser.h"
#include "parser/ZipArchive.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

QByteArray bytes(std::initializer_list<unsigned char> values)
{
    QByteArray out;
    out.resize(static_cast<qsizetype>(values.size()));
    int index = 0;
    for (unsigned char value : values) {
        out[index] = static_cast<char>(value);
        ++index;
    }
    return out;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(data) == data.size();
}

void append16(QByteArray& out, quint16 value)
{
    out.append(static_cast<char>(value & 0xff));
    out.append(static_cast<char>((value >> 8) & 0xff));
}

void append32(QByteArray& out, quint32 value)
{
    append16(out, static_cast<quint16>(value & 0xffff));
    append16(out, static_cast<quint16>(value >> 16));
}

quint32 crc32Of(const QByteArray& data)
{
    quint32 crc = 0xffffffffu;
    for (char byte : data) {
        crc ^= static_cast<unsigned char>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1u) != 0) {
                crc = (crc >> 1) ^ 0xedb88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

struct StoredEntry {
    QString name;
    QByteArray bytes;
    quint32 declaredSize = 0;
};

QByteArray storedZip(const QList<StoredEntry>& entries)
{
    QByteArray out;
    struct Placed {
        QByteArray name;
        quint32 crc = 0;
        quint32 size = 0;
        quint32 offset = 0;
    };
    QList<Placed> placed;
    for (const StoredEntry& entry : entries) {
        Placed item;
        item.name = entry.name.toUtf8();
        item.crc = crc32Of(entry.bytes);
        item.size = entry.declaredSize == 0 ? static_cast<quint32>(entry.bytes.size()) : entry.declaredSize;
        item.offset = static_cast<quint32>(out.size());
        append32(out, 0x04034b50u);
        append16(out, 20);
        append16(out, 0x800);
        append16(out, 0);
        append16(out, 0);
        append16(out, 0);
        append32(out, item.crc);
        append32(out, item.size);
        append32(out, item.size);
        append16(out, static_cast<quint16>(item.name.size()));
        append16(out, 0);
        out.append(item.name);
        out.append(entry.bytes);
        placed.push_back(item);
    }
    const quint32 directoryOffset = static_cast<quint32>(out.size());
    for (const Placed& item : placed) {
        append32(out, 0x02014b50u);
        append16(out, 20);
        append16(out, 20);
        append16(out, 0x800);
        append16(out, 0);
        append16(out, 0);
        append16(out, 0);
        append32(out, item.crc);
        append32(out, item.size);
        append32(out, item.size);
        append16(out, static_cast<quint16>(item.name.size()));
        append16(out, 0);
        append16(out, 0);
        append16(out, 0);
        append16(out, 0);
        append32(out, 0);
        append32(out, item.offset);
        out.append(item.name);
    }
    const quint32 directorySize = static_cast<quint32>(out.size()) - directoryOffset;
    append32(out, 0x06054b50u);
    append16(out, 0);
    append16(out, 0);
    append16(out, static_cast<quint16>(placed.size()));
    append16(out, static_cast<quint16>(placed.size()));
    append32(out, directorySize);
    append32(out, directoryOffset);
    append16(out, 0);
    return out;
}

void append16be(QByteArray& out, quint16 value)
{
    out.append(static_cast<char>((value >> 8) & 0xff));
    out.append(static_cast<char>(value & 0xff));
}

void append32be(QByteArray& out, quint32 value)
{
    append16be(out, static_cast<quint16>((value >> 16) & 0xffff));
    append16be(out, static_cast<quint16>(value & 0xffff));
}

QByteArray nestedTags(const QByteArray& open, const QByteArray& close, const QByteArray& inner, int depth)
{
    QByteArray out;
    out.reserve(open.size() * depth + close.size() * depth + inner.size());
    for (int i = 0; i < depth; ++i) {
        out.append(open);
    }
    out.append(inner);
    for (int i = 0; i < depth; ++i) {
        out.append(close);
    }
    return out;
}

// 未压缩的 Palm DOC。encryption 为 2 时 libmobi 会标成 DRM，但不会调用 mobi_drm_decrypt。
QByteArray palmMobi(const QByteArray& text, quint16 encryption)
{
    constexpr int kRecordBytes = 4096;
    const qsizetype textCount = text.isEmpty() ? 0 : (text.size() + kRecordBytes - 1) / kRecordBytes;
    const int recCount = static_cast<int>(textCount) + 1;
    QByteArray out;
    out.reserve(78 + recCount * 8 + 16 + text.size());
    out.append(QByteArray(32, '\0'));
    append16be(out, 0);
    append16be(out, 0);
    append32be(out, 0);
    append32be(out, 0);
    append32be(out, 0);
    append32be(out, 0);
    append32be(out, 0);
    append32be(out, 0);
    out.append("BOOK", 4);
    out.append("MOBI", 4);
    append32be(out, 0);
    append32be(out, 0);
    append16be(out, static_cast<quint16>(recCount));
    const int dataStart = 78 + recCount * 8;
    int offset = dataStart;
    for (int index = 0; index < recCount; ++index) {
        append32be(out, static_cast<quint32>(offset));
        out.append(static_cast<char>(0));
        out.append(static_cast<char>(0));
        out.append(static_cast<char>(0));
        out.append(static_cast<char>(index & 0xff));
        if (index == 0) {
            offset += 16;
        } else {
            const qsizetype begin = static_cast<qsizetype>(index - 1) * kRecordBytes;
            const qsizetype end = std::min(text.size(), begin + kRecordBytes);
            offset += static_cast<int>(end - begin);
        }
    }
    append16be(out, 1);
    append16be(out, 0);
    append32be(out, static_cast<quint32>(text.size()));
    append16be(out, static_cast<quint16>(textCount));
    append16be(out, static_cast<quint16>(kRecordBytes));
    append16be(out, encryption);
    append16be(out, 0);
    for (qsizetype begin = 0; begin < text.size(); begin += kRecordBytes) {
        const qsizetype end = std::min(text.size(), begin + kRecordBytes);
        out.append(text.constData() + begin, end - begin);
    }
    return out;
}

QByteArray deflatedZip(const QString& name, quint64 uncompressedBytes, quint32 declaredSize)
{
    z_stream stream;
    std::memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    const QByteArray zeros(64 * 1024, '\0');
    QByteArray compressed(64 * 1024, '\0');
    stream.next_out = reinterpret_cast<Bytef*>(compressed.data());
    stream.avail_out = static_cast<uInt>(compressed.size());
    uLong crc = crc32(0L, Z_NULL, 0);
    quint64 left = uncompressedBytes;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stream.avail_in == 0 && left > 0) {
            const uInt chunk = static_cast<uInt>(std::min(left, static_cast<quint64>(zeros.size())));
            crc = crc32(crc, reinterpret_cast<const Bytef*>(zeros.constData()), chunk);
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(zeros.constData()));
            stream.avail_in = chunk;
            left -= chunk;
        }
        if (stream.avail_out == 0) {
            const qsizetype old = compressed.size();
            compressed.resize(old + 64 * 1024);
            stream.next_out = reinterpret_cast<Bytef*>(compressed.data() + old);
            stream.avail_out = static_cast<uInt>(64 * 1024);
        }
        const int flush = (left == 0 && stream.avail_in == 0) ? Z_FINISH : Z_NO_FLUSH;
        status = deflate(&stream, flush);
        if (status != Z_OK && status != Z_STREAM_END) {
            deflateEnd(&stream);
            return {};
        }
    }
    compressed.resize(static_cast<qsizetype>(stream.total_out));
    deflateEnd(&stream);

    const QByteArray entryName = name.toUtf8();
    QByteArray out;
    const auto writeEntry = [&](bool central, quint32 localOffset) {
        append32(out, central ? 0x02014b50u : 0x04034b50u);
        if (central) {
            append16(out, 20);
        }
        append16(out, 20);
        append16(out, 0);
        append16(out, 8);
        append16(out, 0);
        append16(out, 0);
        append32(out, static_cast<quint32>(crc));
        append32(out, static_cast<quint32>(compressed.size()));
        append32(out, declaredSize);
        append16(out, static_cast<quint16>(entryName.size()));
        append16(out, 0);
        if (central) {
            append16(out, 0);
            append16(out, 0);
            append16(out, 0);
            append32(out, 0);
            append32(out, localOffset);
        }
        out.append(entryName);
    };
    writeEntry(false, 0);
    out.append(compressed);
    const quint32 directoryOffset = static_cast<quint32>(out.size());
    writeEntry(true, 0);
    const quint32 directorySize = static_cast<quint32>(out.size()) - directoryOffset;
    append32(out, 0x06054b50u);
    append16(out, 0);
    append16(out, 0);
    append16(out, 1);
    append16(out, 1);
    append32(out, directorySize);
    append32(out, directoryOffset);
    append16(out, 0);
    return out;
}

QString paragraphText(const moyu::ChapterContent& content, const moyu::Paragraph& paragraph)
{
    const qsizetype length = static_cast<qsizetype>(paragraph.byteEnd - paragraph.byteStart);
    return QString::fromUtf8(content.utf8.mid(paragraph.byteStart, length));
}

moyu::Result<moyu::TxtDocument> openIn(const QString& path, const QString& cacheDir, moyu::Database* database = nullptr)
{
    std::atomic<bool> cancel{false};
    return moyu::openBookFile(path, cacheDir, database, 0, 0, cancel, {}, nullptr);
}

moyu::DetectedEncoding encodingOf(const QString& canonical, const QString& iconvName, moyu::NewlineCut cut)
{
    moyu::DetectedEncoding encoding;
    encoding.canonical = canonical;
    encoding.iconvName = iconvName;
    encoding.cut = cut;
    return encoding;
}

}  // namespace

class CoreTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void utf16ChunkDoesNotSplitShang();
    void gbkChunkKeepsCharacterBeforeNewline();
    void transcodeKnownSamples();
    void invalidByteBecomesReplacement();
    void emptyFileIsAnError();
    void utf8BomSkipsHeader();
    void chineseAndVolumeLevels();
    void englishAndAuxiliaryHeadings();
    void longLineIsNotAHeading();
    void denseHeadingsCollapse();
    void whitespacePreambleIsAbsorbed();
    void noHeadingsThenSyntheticSplit();
    void rangesCoverTheBuffer();
    void fingerprintIsStable();
    void databaseKeepsProgressAcrossUpsert();
    void reopenKeepsExplicitZero();
    void asciiPrefixThenGbkIsTranscoded();
    void chunkWithoutNewlineIsCapped();
    void crOnlyNewlinesSplitChapters();
    void overlongParagraphIsSplit();
    void uiSaveDoesNotWaitWhileChaptersWrite();
    void samePathUpdateKeepsOneRow();
    void auxiliaryHeadingsMergeIntoChineseRule();
    void splitVolumePiecesDropToLevelTwo();
    void big5HkscsDecodesExtension();
    void utf8CacheSkipsTranscodeWhenRowsAreGone();
    void searchFindsNeedleAcrossChunkBoundary();
    void searchCanCancelBeforeItStarts();
    void searchFiveMillionCharacters();
    void headingMarkIsNotShown();
    void epub2SpineAndNcxBecomeChapters();
    void epub3NavTitlesTheSpine();
    void epubReopenUsesUtf8Cache();
    void corruptEpubDoesNotCrash();
    void zipEntryLargerThanCapIsRejected();
    void encryptedEpubIsRejected();
    void fontObfuscationIsNotDrm();
    void epubPathTraversalIsRejected();
    void htmlHeadingsBecomeChapters();
    void markdownHeadingsBecomeChapters();
    void fb2SectionsBecomeChapters();
    void corruptMobiDoesNotCrash();
    void unsupportedExtensionIsRejected();
    void deepMarkupDoesNotCrash();
    void ampersandRunFinishesQuickly();
    void selfClosingScriptKeepsFollowingText();
    void legalDotDotPathOpens();
    void remoteManifestItemIsSkipped();
    void missingSpineItemIsSkipped();
    void fb2InlineTagsKeepText();
    void namedEntitiesDecodeOnBothPaths();
    void epubFragmentSplitsChapters();
    void markdownFenceIsNotAChapter();
    void htmlMetaCharsetWins();
    void encryptionWithoutMethodIsDrm();
    void deflateBombIsRejected();
    void truncatedCentralDirectoryDoesNotCrash();
    void encryptedMobiHeaderIsRejected();
    void singleMobiPartCanExceedThirtyTwoMegabytes();
    void customRegexBecomesChapters();
    void customRegexIgnoresLongLines();
    void invalidChapterRegexIsAnError();
    void customRegexFallsBackToBuiltin();
    void invalidRegexStillOpensTxt();
    void shelfListsNewestFirst();
    void deleteBookRemovesRecordAndCache();
    void retargetKeepsProgress();
};

void CoreTest::utf16ChunkDoesNotSplitShang()
{
    // 上 \n 好，UTF-16LE。按字节搜 0x0A 会在「上」中间切开。
    const QByteArray data = bytes({0x0A, 0x4E, 0x0A, 0x00, 0x7D, 0x59});
    const char* begin = data.constData();
    const char* end = begin + data.size();
    QCOMPARE(moyu::findChunkEnd(begin, end, 1, moyu::NewlineCut::Utf16Le), 4ULL);
    QCOMPARE(moyu::findChunkEnd(begin, end, 2, moyu::NewlineCut::Utf16Le), 4ULL);
    QCOMPARE(moyu::findChunkEnd(begin, end, 100, moyu::NewlineCut::Utf16Le), 4ULL);
}

void CoreTest::gbkChunkKeepsCharacterBeforeNewline()
{
    // GB18030「你」是 C4 E3，后面才是换行。窗口只有 1 字节时也要延伸到换行。
    const QByteArray data = bytes({0xC4, 0xE3, 0x0A, 0xBA, 0xC3});
    QCOMPARE(moyu::findChunkEnd(data.constData(), data.constData() + data.size(), 1, moyu::NewlineCut::Bytes), 3ULL);
}

void CoreTest::transcodeKnownSamples()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    std::atomic<bool> cancel{false};
    const QString path = dir.filePath(QStringLiteral("out.utf8"));

    const auto convert = [&](const QByteArray& input, const moyu::DetectedEncoding& encoding, QByteArray* output) {
        QFile::remove(path);
        auto written = moyu::transcodeToCacheFile(input.constData(),
                                                   static_cast<quint64>(input.size()),
                                                   encoding,
                                                   path,
                                                   1,
                                                   cancel);
        if (!written) {
            return written.error().message;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return QStringLiteral("无法读取缓存");
        }
        *output = file.readAll();
        return QString();
    };

    QByteArray hello;
    QCOMPARE(convert(bytes({0xC4, 0xE3, 0xBA, 0xC3}),
                     encodingOf(QStringLiteral("GB18030"), QStringLiteral("GB18030"), moyu::NewlineCut::Bytes),
                     &hello),
             QString());
    QCOMPARE(hello, bytes({0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD}));

    QByteArray euro;
    QCOMPARE(convert(bytes({0xC4, 0xE3, 0xBA, 0xC3, 0xA2, 0xE3}),
                     encodingOf(QStringLiteral("GB18030"), QStringLiteral("GB18030"), moyu::NewlineCut::Bytes),
                     &euro),
             QString());
    QCOMPARE(euro, bytes({0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD, 0xE2, 0x82, 0xAC}));

    QByteArray big5;
    QCOMPARE(convert(bytes({0xB4, 0xFA, 0xB8, 0xD5}),
                     encodingOf(QStringLiteral("BIG5"), QStringLiteral("BIG5"), moyu::NewlineCut::Bytes),
                     &big5),
             QString());
    QCOMPARE(big5, bytes({0xE6, 0xB8, 0xAC, 0xE8, 0xA9, 0xA6}));

    QByteArray utf16;
    QCOMPARE(convert(bytes({0x0A, 0x4E, 0x0A, 0x00, 0x7D, 0x59}),
                     encodingOf(QStringLiteral("UTF-16LE"), QStringLiteral("UTF-16LE"), moyu::NewlineCut::Utf16Le),
                     &utf16),
             QString());
    QCOMPARE(utf16, bytes({0xE4, 0xB8, 0x8A, 0x0A, 0xE5, 0xA5, 0xBD}));
}

void CoreTest::invalidByteBecomesReplacement()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    std::atomic<bool> cancel{false};
    const QByteArray input = bytes({0xFF});
    const QString path = dir.filePath(QStringLiteral("bad.utf8"));
    auto written = moyu::transcodeToCacheFile(input.constData(),
                                               1,
                                               encodingOf(QStringLiteral("GB18030"), QStringLiteral("GB18030"), moyu::NewlineCut::Bytes),
                                               path,
                                               64,
                                               cancel);
    QVERIFY(written);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), bytes({0xEF, 0xBF, 0xBD}));
}

void CoreTest::emptyFileIsAnError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("empty.txt"));
    QVERIFY(writeFile(path, QByteArray()));
    auto mapped = moyu::MappedFile::open(path);
    QVERIFY(!mapped);
    std::atomic<bool> cancel{false};
    auto opened = moyu::openTxtFile(path, dir.path(), nullptr, 0, 0, cancel, {}, nullptr);
    QVERIFY(!opened);
}

void CoreTest::utf8BomSkipsHeader()
{
    const QByteArray data = bytes({0xEF, 0xBB, 0xBF, 0xE4, 0xBD, 0xA0});
    auto detected = moyu::detectEncoding(data.constData(), static_cast<quint64>(data.size()));
    QVERIFY(detected);
    QCOMPARE(detected->canonical, QStringLiteral("UTF-8"));
    QCOMPARE(detected->bodyOffset, 3ULL);
    QVERIFY(detected->passthroughUtf8());
}

void CoreTest::chineseAndVolumeLevels()
{
    const QByteArray text = QStringLiteral("第一卷 起源\n正文\n第1章 甲\n更多\n").toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(text.constData(), static_cast<quint64>(text.size()), &cancel);
    QVERIFY(chapters);
    QCOMPARE(chapters->size(), 2U);
    QCOMPARE(chapters->at(0).title, QStringLiteral("第一卷 起源"));
    QCOMPARE(chapters->at(0).level, static_cast<quint8>(1));
    QCOMPARE(chapters->at(1).title, QStringLiteral("第1章 甲"));
    QCOMPARE(chapters->at(1).level, static_cast<quint8>(2));
    QVERIFY(moyu::chapterRangesValid(*chapters, static_cast<quint64>(text.size())));
}

void CoreTest::englishAndAuxiliaryHeadings()
{
    const QByteArray english = QStringLiteral("Chapter 1\nhello\nCHAPTER IV\nworld\n").toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(english.constData(), static_cast<quint64>(english.size()), &cancel);
    QVERIFY(chapters);
    QCOMPARE(chapters->size(), 2U);
    QCOMPARE(chapters->at(0).title, QStringLiteral("Chapter 1"));
    QCOMPARE(chapters->at(1).title, QStringLiteral("CHAPTER IV"));

    const QByteArray aux = QStringLiteral("序章\n正文一段\n楔子 开始\n后文\n").toUtf8();
    auto auxChapters = moyu::detectChapters(aux.constData(), static_cast<quint64>(aux.size()), &cancel);
    QVERIFY(auxChapters);
    QCOMPARE(auxChapters->size(), 2U);
    QCOMPARE(auxChapters->at(0).title, QStringLiteral("序章"));
    QCOMPARE(auxChapters->at(1).title, QStringLiteral("楔子 开始"));
    QCOMPARE(auxChapters->at(0).level, static_cast<quint8>(2));
}

void CoreTest::longLineIsNotAHeading()
{
    const QString longLine = QStringLiteral("第1章 ") + QString(41, QChar(u'字'));
    const QByteArray text = (longLine + QStringLiteral("\n第2章 短\n正文\n")).toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(text.constData(), static_cast<quint64>(text.size()), &cancel);
    QVERIFY(chapters);
    QCOMPARE(chapters->size(), 2U);
    QCOMPARE(chapters->at(0).title, QStringLiteral("开头"));
    QCOMPARE(chapters->at(1).title, QStringLiteral("第2章 短"));
}

void CoreTest::denseHeadingsCollapse()
{
    QString text;
    for (int i = 0; i < 10; ++i) {
        text += QStringLiteral("第1章\n");
    }
    const QByteArray utf8 = text.toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(utf8.constData(), static_cast<quint64>(utf8.size()), &cancel);
    QVERIFY(chapters);
    QCOMPARE(chapters->size(), 1U);
    QCOMPARE(chapters->at(0).title, QStringLiteral("正文"));
}

void CoreTest::whitespacePreambleIsAbsorbed()
{
    const QByteArray text = QStringLiteral("\n\n第1章 甲\n正文\n").toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(text.constData(), static_cast<quint64>(text.size()), &cancel);
    QVERIFY(chapters);
    QCOMPARE(chapters->size(), 1U);
    QCOMPARE(chapters->front().byteStart, 0ULL);
    QCOMPARE(chapters->front().title, QStringLiteral("第1章 甲"));
}

void CoreTest::noHeadingsThenSyntheticSplit()
{
    QString text(100001, QLatin1Char('a'));
    const QByteArray utf8 = text.toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(utf8.constData(), static_cast<quint64>(utf8.size()), &cancel);
    QVERIFY(chapters);
    QVERIFY(chapters->size() > 1U);
    QCOMPARE(chapters->front().title, QStringLiteral("正文"));
    QVERIFY(!chapters->front().synthetic);
    QCOMPARE(chapters->at(1).title, QStringLiteral("正文 · 2"));
    QVERIFY(chapters->at(1).synthetic);
    QVERIFY(moyu::chapterRangesValid(*chapters, static_cast<quint64>(utf8.size())));
}

void CoreTest::rangesCoverTheBuffer()
{
    const QByteArray text = QStringLiteral("前文\n第1章 甲\n中间\n第2章 乙\n结尾\n").toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(text.constData(), static_cast<quint64>(text.size()), &cancel);
    QVERIFY(chapters);
    QVERIFY(moyu::chapterRangesValid(*chapters, static_cast<quint64>(text.size())));
    QCOMPARE(chapters->front().title, QStringLiteral("开头"));
}

void CoreTest::fingerprintIsStable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("a.txt"));
    QVERIFY(writeFile(path, QByteArray("fingerprint-sample")));
    auto first = moyu::fingerprintOf(path);
    auto second = moyu::fingerprintOf(path);
    QVERIFY(first);
    QVERIFY(second);
    QCOMPARE(first->token(), second->token());
    QVERIFY(!first->token().contains(QLatin1Char(':')));
}

void CoreTest::databaseKeepsProgressAcrossUpsert()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    moyu::Database database(dir.filePath(QStringLiteral("moyu.db")));
    QVERIFY(database.startAndWait());

    moyu::Book book;
    book.filePath = dir.filePath(QStringLiteral("a.txt"));
    book.fingerprint = QStringLiteral("size_mtime_hash");
    book.title = QStringLiteral("A");
    book.format = moyu::BookFormat::Txt;
    book.sourceEncoding = QStringLiteral("UTF-8");
    book.totalBytes = 10;
    book.lastReadAt = 1;
    auto id = database.upsertBook(book);
    QVERIFY2(static_cast<bool>(id), id ? "" : id.error().message.toUtf8().constData());

    moyu::ChapterIndex chapter;
    chapter.index = 0;
    chapter.title = QStringLiteral("正文");
    chapter.level = 2;
    chapter.byteStart = 0;
    chapter.byteEnd = 10;
    auto replaced = database.replaceChapters(*id, {chapter});
    QVERIFY(replaced);

    moyu::ReadingProgress progress;
    progress.bookId = *id;
    progress.chapterIndex = 3;
    progress.byteOffsetInChapter = 20;
    progress.updatedAt = 5;
    QVERIFY(database.saveProgress(progress));

    book.title = QStringLiteral("B");
    book.lastReadAt = 9;
    auto again = database.upsertBook(book);
    QVERIFY(again);
    QCOMPARE(*again, *id);

    auto loaded = database.findByFingerprint(book.fingerprint);
    QVERIFY(loaded);
    QVERIFY(loaded->has_value());
    QCOMPARE((*loaded)->book.title, QStringLiteral("B"));
    QVERIFY((*loaded)->progress.has_value());
    QCOMPARE((*loaded)->progress->chapterIndex, 3U);
    QCOMPARE((*loaded)->progress->byteOffsetInChapter, 20U);
    QCOMPARE((*loaded)->chapters.size(), 1U);
    QCOMPARE((*loaded)->chapters.front().title, QStringLiteral("正文"));
}

void CoreTest::reopenKeepsExplicitZero()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("story.txt"));
    const QByteArray text = QStringLiteral("第1章 甲\n你好\n第2章 乙\n世界\n").toUtf8();
    QVERIFY(writeFile(path, text));

    moyu::Database database(dir.filePath(QStringLiteral("moyu.db")));
    QVERIFY(database.startAndWait());
    std::atomic<bool> cancel{false};
    auto first = moyu::openTxtFile(path, dir.path(), &database, 0, 0, cancel, {}, nullptr);
    QVERIFY2(static_cast<bool>(first), first ? "" : first.error().message.toUtf8().constData());
    QCOMPARE(first->chapterIndex, 0U);
    QVERIFY(!first->cacheHit);

    moyu::ReadingProgress progress;
    progress.bookId = first->parsed.book.id;
    progress.chapterIndex = 1;
    progress.byteOffsetInChapter = 0;
    progress.updatedAt = 1;
    QVERIFY(database.saveProgress(progress));

    auto second = moyu::openTxtFile(path, dir.path(), &database, 0, 0, cancel, {}, nullptr);
    QVERIFY(second);
    QVERIFY(second->cacheHit);
    QCOMPARE(second->chapterIndex, 0U);

    auto third = moyu::openTxtFile(path, dir.path(), &database, 1, 0, cancel, {}, nullptr);
    QVERIFY(third);
    QCOMPARE(third->chapterIndex, 1U);
    QCOMPARE(third->chapter.title, QStringLiteral("第2章 乙"));
}

void CoreTest::asciiPrefixThenGbkIsTranscoded()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QByteArray data;
    const QByteArray line = QByteArrayLiteral("https://example.com/novel/chapter\n");
    while (data.size() < 70 * 1024) {
        data += line;
    }
    data += bytes({0xC4, 0xE3, 0xBA, 0xC3, 0x0A});
    const QString path = dir.filePath(QStringLiteral("mixed.txt"));
    QVERIFY(writeFile(path, data));

    std::atomic<bool> cancel{false};
    auto opened = moyu::openTxtFile(path, dir.path(), nullptr, 0, 0, cancel, {}, nullptr);
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(opened->parsed.book.sourceEncoding, QStringLiteral("GB18030"));
    QVERIFY2(opened->chapter.utf8.contains(QStringLiteral("你好").toUtf8()),
             opened->chapter.utf8.right(16).toHex().constData());
    QVERIFY(!opened->chapter.utf8.contains(QByteArray("\xEF\xBF\xBD")));
    const QString cache = moyu::utf8CachePath(dir.path(), opened->parsed.book.fingerprint);
    QFile cached(cache);
    QVERIFY2(cached.open(QIODevice::ReadOnly), "误判成 UTF-8 时不会写出转码缓存");
    QVERIFY(cached.readAll().contains(QStringLiteral("你好").toUtf8()));
}

void CoreTest::chunkWithoutNewlineIsCapped()
{
    const QByteArray data(2 * 1024 * 1024, 'A');
    const char* begin = data.constData();
    const quint64 cut = moyu::findChunkEnd(begin, begin + data.size(), 1024, moyu::NewlineCut::Bytes);
    QVERIFY2(cut <= static_cast<quint64>(1024 * 1024), qPrintable(QString::number(cut)));
    QVERIFY(cut < static_cast<quint64>(data.size()));
}

void CoreTest::crOnlyNewlinesSplitChapters()
{
    const QByteArray text = QStringLiteral("第1章 甲\r正文\r第2章 乙\r世界\r").toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(text.constData(), static_cast<quint64>(text.size()), &cancel);
    QVERIFY(chapters);
    QCOMPARE(chapters->size(), 2U);
    QCOMPARE(chapters->at(0).title, QStringLiteral("第1章 甲"));
    QCOMPARE(chapters->at(1).title, QStringLiteral("第2章 乙"));
}

void CoreTest::overlongParagraphIsSplit()
{
    const QString text(5000, QChar(u'文'));
    const QByteArray utf8 = text.toUtf8();
    const auto paragraphs = moyu::splitParagraphs(utf8.constData(), static_cast<quint64>(utf8.size()));
    QVERIFY(paragraphs.size() > 1U);
    for (const moyu::Paragraph& paragraph : paragraphs) {
        QVERIFY(paragraph.codePoints <= 2000);
        QVERIFY(paragraph.codePoints > 0);
    }
}

void CoreTest::uiSaveDoesNotWaitWhileChaptersWrite()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    moyu::Database database(dir.filePath(QStringLiteral("moyu.db")));
    QVERIFY(database.startAndWait());

    moyu::Book book;
    book.filePath = dir.filePath(QStringLiteral("a.txt"));
    book.fingerprint = QStringLiteral("fp");
    book.title = QStringLiteral("A");
    book.format = moyu::BookFormat::Txt;
    book.sourceEncoding = QStringLiteral("UTF-8");
    book.totalBytes = 10;
    book.lastReadAt = 1;
    auto id = database.upsertBook(book);
    QVERIFY(id);

    // 另开一条连接占住写锁，模拟数据库线程正在写几千条章节。
    // 界面上的保存如果要等这条锁，就会卡到 busy_timeout。
    const QString connection = QStringLiteral("chapter-write-hold");
    QSqlDatabase hold = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    hold.setDatabaseName(dir.filePath(QStringLiteral("moyu.db")));
    QVERIFY(hold.open());
    moyu::ReadingProgress progress;
    progress.bookId = *id;
    progress.chapterIndex = 1;
    progress.byteOffsetInChapter = 2;
    progress.updatedAt = 7;

    bool sawSetting = false;
    QString settingValue;
    qint64 elapsed = 0;
    {
        QSqlQuery lock(hold);
        QVERIFY(lock.exec(QStringLiteral("BEGIN IMMEDIATE")));
        QElapsedTimer timer;
        timer.start();
        database.saveProgressAsync(progress);
        database.setSettingsAsync({{QStringLiteral("window/x"), QStringLiteral("12")}});
        database.requestSettings(this, {QStringLiteral("window/x")}, [&](const std::vector<std::pair<QString, QString>>& items) {
            if (!items.empty()) {
                settingValue = items.front().second;
            }
            sawSetting = true;
        });
        elapsed = timer.elapsed();
        QVERIFY(lock.exec(QStringLiteral("ROLLBACK")));
        lock.finish();
    }
    hold.close();
    hold = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
    QVERIFY2(elapsed < 50, qPrintable(QStringLiteral("界面保存等了 %1 ms").arg(elapsed)));
    QTRY_VERIFY(sawSetting);
    QCOMPARE(settingValue, QStringLiteral("12"));

    auto loaded = database.findByFingerprint(QStringLiteral("fp"));
    QVERIFY(loaded);
    QVERIFY(loaded->has_value());
    QVERIFY((*loaded)->progress.has_value());
    QCOMPARE((*loaded)->progress->chapterIndex, 1U);
    QCOMPARE((*loaded)->progress->byteOffsetInChapter, 2U);
}

void CoreTest::samePathUpdateKeepsOneRow()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("moyu.db"));
    moyu::Database database(dbPath);
    QVERIFY(database.startAndWait());

    const QString bookPath = dir.filePath(QStringLiteral("serial.txt"));
    moyu::Book book;
    book.filePath = bookPath;
    book.fingerprint = QStringLiteral("fp-old");
    book.title = QStringLiteral("连载");
    book.format = moyu::BookFormat::Txt;
    book.sourceEncoding = QStringLiteral("UTF-8");
    book.totalBytes = 1000;
    book.lastReadAt = 1;
    auto id = database.upsertBook(book);
    QVERIFY(id);

    moyu::ReadingProgress progress;
    progress.bookId = *id;
    progress.chapterIndex = 4;
    progress.byteOffsetInChapter = 80;
    progress.updatedAt = 2;
    QVERIFY(database.saveProgress(progress));

    book.fingerprint = QStringLiteral("fp-new");
    book.totalBytes = 30;
    book.lastReadAt = 9;
    auto again = database.upsertBook(book);
    QVERIFY(again);
    QCOMPARE(*again, *id);

    auto loaded = database.findByFingerprint(QStringLiteral("fp-new"));
    QVERIFY(loaded);
    QVERIFY(loaded->has_value());
    QCOMPARE((*loaded)->book.id, *id);
    QCOMPARE((*loaded)->book.filePath, bookPath);
    QVERIFY((*loaded)->progress.has_value());
    QCOMPARE((*loaded)->progress->chapterIndex, 4U);
    QCOMPARE((*loaded)->progress->byteOffsetInChapter, 80U);

    auto stale = database.findByFingerprint(QStringLiteral("fp-old"));
    QVERIFY(stale);
    QVERIFY(!stale->has_value());

    QSqlDatabase check = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("path-count"));
    check.setDatabaseName(dbPath);
    QVERIFY(check.open());
    QSqlQuery query(check);
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM books")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
    query.finish();
    check.close();
    check = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("path-count"));
}

void CoreTest::auxiliaryHeadingsMergeIntoChineseRule()
{
    const QByteArray text = QStringLiteral(
                                "楔子\n引文\n"
                                "第一章 甲\n正文甲\n"
                                "第二章 乙\n正文乙\n"
                                "第三章 丙\n正文丙\n"
                                "后记\n完\n"
                                "番外\n外一章\n")
                                .toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(text.constData(), static_cast<quint64>(text.size()), &cancel);
    QVERIFY(chapters);
    QStringList titles;
    for (const moyu::ChapterIndex& chapter : *chapters) {
        titles.push_back(chapter.title);
    }
    QCOMPARE(titles, QStringList({QStringLiteral("楔子"),
                                  QStringLiteral("第一章 甲"),
                                  QStringLiteral("第二章 乙"),
                                  QStringLiteral("第三章 丙"),
                                  QStringLiteral("后记"),
                                  QStringLiteral("番外")}));
}

void CoreTest::splitVolumePiecesDropToLevelTwo()
{
    QString text = QStringLiteral("第一卷 长卷\n");
    text += QString(120000, QChar(u'文'));
    const QByteArray utf8 = text.toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(utf8.constData(), static_cast<quint64>(utf8.size()), &cancel);
    QVERIFY(chapters);
    QVERIFY(chapters->size() > 2U);
    QCOMPARE(chapters->front().level, static_cast<quint8>(1));
    QVERIFY(!chapters->front().synthetic);
    QCOMPARE(chapters->at(1).level, static_cast<quint8>(2));
    QVERIFY(chapters->at(1).synthetic);
    QCOMPARE(chapters->back().level, static_cast<quint8>(2));
}

void CoreTest::big5HkscsDecodesExtension()
{
    QByteArray data;
    for (int i = 0; i < 4000; ++i) {
        data += bytes({0xA7, 0x41, 0xA6, 0x6E});
    }
    data += bytes({0xA3, 0xE1});
    auto detected = moyu::detectEncoding(data.constData(), static_cast<quint64>(data.size()));
    QVERIFY(detected);
    QCOMPARE(detected->canonical, QStringLiteral("BIG5"));
    QCOMPARE(detected->iconvName, QStringLiteral("BIG5-HKSCS"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    std::atomic<bool> cancel{false};
    const QString path = dir.filePath(QStringLiteral("out.utf8"));
    auto written = moyu::transcodeToCacheFile(data.constData(),
                                               static_cast<quint64>(data.size()),
                                               *detected,
                                               path,
                                               1024,
                                               cancel);
    QVERIFY2(static_cast<bool>(written), written ? "" : written.error().message.toUtf8().constData());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray utf8 = file.readAll();
    QVERIFY(utf8.contains(QStringLiteral("你好").toUtf8()));
    QVERIFY(utf8.contains(QByteArray("\xE2\x82\xAC")));
    QVERIFY(!utf8.contains(QByteArray("\xEF\xBF\xBD")));
}

void CoreTest::utf8CacheSkipsTranscodeWhenRowsAreGone()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // GBK：第1章 甲，后面重复「你好」。样本太短时 uchardet 认不出编码。
    QByteArray gbk = bytes({0xB5, 0xDA, '1', 0xD5, 0xC2, ' ', 0xBC, 0xD7, 0x0A});
    const QByteArray line = bytes({0xC4, 0xE3, 0xBA, 0xC3, 0x0A});
    while (gbk.size() < 8192) {
        gbk += line;
    }
    const QString path = dir.filePath(QStringLiteral("gbk.txt"));
    QVERIFY(writeFile(path, gbk));
    const QString dbPath = dir.filePath(QStringLiteral("moyu.db"));
    moyu::Database database(dbPath);
    QVERIFY(database.startAndWait());
    std::atomic<bool> cancel{false};
    moyu::resetTranscodeCallCount();
    auto first = moyu::openTxtFile(path, dir.path(), &database, 0, 0, cancel, {}, nullptr);
    QVERIFY2(static_cast<bool>(first), first ? "" : first.error().message.toUtf8().constData());
    QVERIFY(!first->cacheHit);
    QCOMPARE(moyu::transcodeCallCount(), 1ULL);
    QVERIFY(!first->parsed.chapters.empty());
    const QString cache = moyu::utf8CachePath(dir.path(), first->parsed.book.fingerprint);
    const QFileInfo cacheInfo(cache);
    QVERIFY(cacheInfo.exists());
    const qint64 cacheBytes = cacheInfo.size();
    QVERIFY(cacheBytes > 0);
    const auto chapterCount = first->parsed.chapters.size();

    {
        QSqlDatabase wipe = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("wipe-cache"));
        wipe.setDatabaseName(dbPath);
        QVERIFY(wipe.open());
        QSqlQuery query(wipe);
        QVERIFY(query.exec(QStringLiteral("PRAGMA foreign_keys=ON")));
        QVERIFY(query.exec(QStringLiteral("DELETE FROM books")));
        query.finish();
        wipe.close();
        wipe = QSqlDatabase();
        QSqlDatabase::removeDatabase(QStringLiteral("wipe-cache"));
    }
    auto gone = database.findByFingerprint(first->parsed.book.fingerprint);
    QVERIFY(gone);
    QVERIFY(!gone->has_value());

    moyu::resetTranscodeCallCount();
    auto second = moyu::openTxtFile(path, dir.path(), &database, 0, 0, cancel, {}, nullptr);
    QVERIFY2(static_cast<bool>(second), second ? "" : second.error().message.toUtf8().constData());
    QCOMPARE(moyu::transcodeCallCount(), 0ULL);
    QVERIFY(!second->cacheHit);
    QCOMPARE(second->parsed.chapters.size(), chapterCount);
    QCOMPARE(second->chapter.utf8.contains(QStringLiteral("你好").toUtf8()), true);
    QCOMPARE(QFileInfo(cache).size(), cacheBytes);
}

void CoreTest::searchFindsNeedleAcrossChunkBoundary()
{
    const QByteArray data = QByteArrayLiteral("xxxxEFGHyyyy");
    std::atomic<bool> cancel{false};
    std::vector<moyu::SearchHit> hits;
    moyu::SearchTiming timing;
    auto found = moyu::searchUtf8(data.constData(),
                                   static_cast<quint64>(data.size()),
                                   QByteArrayLiteral("EFGH"),
                                   6,
                                   cancel,
                                   [&hits](const std::vector<moyu::SearchHit>& batch) {
                                       hits.insert(hits.end(), batch.begin(), batch.end());
                                   },
                                   &timing);
    QVERIFY(found);
    QCOMPARE(hits.size(), 1U);
    QCOMPARE(hits.front().byteOffset, 4ULL);
}

void CoreTest::searchCanCancelBeforeItStarts()
{
    const QByteArray data(1 << 20, 'a');
    std::atomic<bool> cancel{true};
    moyu::SearchTiming timing;
    int batches = 0;
    auto found = moyu::searchUtf8(data.constData(),
                                   static_cast<quint64>(data.size()),
                                   QByteArrayLiteral("a"),
                                   1 << 16,
                                   cancel,
                                   [&batches](const std::vector<moyu::SearchHit>&) { ++batches; },
                                   &timing);
    QVERIFY(found);
    QVERIFY(timing.cancelled);
    QCOMPARE(batches, 0);
    QCOMPARE(timing.hits, 0ULL);
}

void CoreTest::searchFiveMillionCharacters()
{
    constexpr int kCharacters = 5000000;
    const QByteArray unit = QStringLiteral("字").toUtf8();
    QCOMPARE(unit.size(), 3);
    QByteArray data(static_cast<qsizetype>(kCharacters) * unit.size(), '\0');
    for (int i = 0; i < kCharacters; ++i) {
        std::memcpy(data.data() + static_cast<qsizetype>(i) * unit.size(), unit.constData(), static_cast<std::size_t>(unit.size()));
    }
    const QByteArray needle = QStringLiteral("寻得").toUtf8();
    const auto plant = [&](int character) {
        const qsizetype at = static_cast<qsizetype>(character) * unit.size();
        data.replace(at, needle.size(), needle);
    };
    plant(40);
    plant(kCharacters / 2);
    plant(kCharacters - 80);

    std::atomic<bool> cancel{false};
    moyu::SearchTiming timing;
    auto found = moyu::searchUtf8(data.constData(),
                                   static_cast<quint64>(data.size()),
                                   needle,
                                   1ULL << 18,
                                   cancel,
                                   [](const std::vector<moyu::SearchHit>&) {},
                                   &timing);
    QVERIFY(found);
    QCOMPARE(timing.hits, 3ULL);
    QVERIFY(timing.firstBatchMs >= 0);
    std::fprintf(stderr,
                 "search_5m_first_ms %.3f total_ms %.3f hits %llu\n",
                 timing.firstBatchMs,
                 timing.totalMs,
                 static_cast<unsigned long long>(timing.hits));
    QVERIFY2(timing.firstBatchMs < 300.0,
             qPrintable(QStringLiteral("首个结果 %1 ms").arg(timing.firstBatchMs)));
}

QByteArray epubPackage(const QByteArray& opf, const QByteArray& extraName, const QByteArray& extraBytes)
{
    QList<StoredEntry> entries;
    entries.push_back({QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"), 0});
    entries.push_back({QStringLiteral("META-INF/container.xml"),
                       QByteArrayLiteral("<?xml version=\"1.0\"?>"
                                         "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                                         "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
                                         "media-type=\"application/oebps-package+xml\"/></rootfiles></container>"),
                       0});
    entries.push_back({QStringLiteral("OEBPS/content.opf"), opf, 0});
    entries.push_back({QStringLiteral("OEBPS/c1.xhtml"),
                       QByteArrayLiteral("<?xml version=\"1.0\"?>"
                                         "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
                                         "<h1>章内标题</h1><p>甲段有<b>粗体</b></p></body></html>"),
                       0});
    entries.push_back({QStringLiteral("OEBPS/c2.xhtml"),
                       QByteArrayLiteral("<?xml version=\"1.0\"?>"
                                         "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>乙段</p></body></html>"),
                       0});
    if (!extraName.isEmpty()) {
        entries.push_back({QString::fromUtf8(extraName), extraBytes, 0});
    }
    return storedZip(entries);
}

const QByteArray kEpub2Opf =
    QByteArrayLiteral("<?xml version=\"1.0\"?>"
                      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uid\">"
                      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                      "<dc:title>测试书</dc:title><dc:creator>作者甲</dc:creator>"
                      "<dc:language>zh</dc:language><dc:identifier id=\"uid\">id</dc:identifier></metadata>"
                      "<manifest>"
                      "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
                      "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
                      "<item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>"
                      "</manifest><spine toc=\"ncx\"><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>");

const QByteArray kEpub2Ncx =
    QByteArrayLiteral("<?xml version=\"1.0\"?>"
                      "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><navMap>"
                      "<navPoint id=\"n1\"><navLabel><text>第一章</text></navLabel><content src=\"c1.xhtml\"/></navPoint>"
                      "<navPoint id=\"n2\"><navLabel><text>第二章</text></navLabel><content src=\"c2.xhtml\"/></navPoint>"
                      "</navMap></ncx>");

void CoreTest::headingMarkIsNotShown()
{
    QByteArray utf8;
    utf8.append(moyu::kHeadingMark);
    utf8.append(QStringLiteral("标题\n").toUtf8());
    utf8.append(moyu::kHeadingMark);
    utf8.append('\n');
    utf8.append(QStringLiteral("正文").toUtf8());
    const std::vector<moyu::Paragraph> paragraphs = moyu::splitParagraphs(utf8.constData(), static_cast<quint64>(utf8.size()));
    QCOMPARE(paragraphs.size(), 2U);
    QCOMPARE(paragraphs[0].kind, moyu::ParagraphKind::Heading);
    QCOMPARE(paragraphs[1].kind, moyu::ParagraphKind::Body);
    moyu::ChapterContent content;
    content.utf8 = utf8;
    QCOMPARE(paragraphText(content, paragraphs[0]), QStringLiteral("标题"));
    QCOMPARE(paragraphText(content, paragraphs[1]), QStringLiteral("正文"));
    QVERIFY(!paragraphText(content, paragraphs[0]).contains(QChar(static_cast<char16_t>(static_cast<unsigned char>(moyu::kHeadingMark)))));
}

void CoreTest::epub2SpineAndNcxBecomeChapters()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray zip = epubPackage(kEpub2Opf, QByteArrayLiteral("OEBPS/toc.ncx"), kEpub2Ncx);
    const QString path = dir.filePath(QStringLiteral("book.epub"));
    QVERIFY(writeFile(path, zip));
    moyu::resetTranscodeCallCount();
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(moyu::transcodeCallCount(), 0ULL);
    QCOMPARE(opened->parsed.book.format, moyu::BookFormat::Epub);
    QCOMPARE(opened->parsed.book.title, QStringLiteral("测试书"));
    QCOMPARE(opened->parsed.book.author, QStringLiteral("作者甲"));
    QCOMPARE(opened->parsed.book.sourceEncoding, QStringLiteral("UTF-8"));
    QCOMPARE(opened->parsed.chapters.size(), 2U);
    QCOMPARE(opened->parsed.chapters[0].title, QStringLiteral("第一章"));
    QCOMPARE(opened->parsed.chapters[1].title, QStringLiteral("第二章"));
    QVERIFY(opened->parsed.bodyPath.endsWith(QStringLiteral(".utf8")));
    QVERIFY(opened->chapter.utf8.contains(moyu::kHeadingMark));
    bool sawBody = false;
    bool sawHeading = false;
    for (const moyu::Paragraph& paragraph : opened->chapter.paragraphs) {
        const QString text = paragraphText(opened->chapter, paragraph);
        QVERIFY(!text.contains(QChar(static_cast<char16_t>(static_cast<unsigned char>(moyu::kHeadingMark)))));
        QVERIFY(!text.contains(QLatin1Char('<')));
        if (text.contains(QStringLiteral("甲段有粗体"))) {
            sawBody = true;
            QCOMPARE(paragraph.kind, moyu::ParagraphKind::Body);
        }
        if (text == QStringLiteral("章内标题")) {
            sawHeading = true;
            QCOMPARE(paragraph.kind, moyu::ParagraphKind::Heading);
        }
    }
    QVERIFY(sawBody);
    QVERIFY(sawHeading);
    auto second = moyu::readMappedChapter(opened->parsed.bodyPath, opened->parsed.bodyOffset, opened->parsed.chapters[1]);
    QVERIFY(second);
    QVERIFY(second->utf8.contains(QStringLiteral("乙段").toUtf8()));
}

void CoreTest::epub3NavTitlesTheSpine()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray opf = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"uid\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>导航书</dc:title><dc:language>zh</dc:language><dc:identifier id=\"uid\">id</dc:identifier>"
        "</metadata><manifest>"
        "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>"
        "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
        "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "</manifest><spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>");
    const QByteArray nav = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><body>"
        "<nav epub:type=\"toc\"><ol><li><a href=\"c1.xhtml\">甲章</a></li>"
        "<li><a href=\"c2.xhtml\">乙章</a></li></ol></nav></body></html>");
    const QByteArray ncx = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><navMap>"
        "<navPoint id=\"n1\"><navLabel><text>不该用</text></navLabel><content src=\"c1.xhtml\"/></navPoint>"
        "</navMap></ncx>");
    QList<StoredEntry> entries;
    entries.push_back({QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"), 0});
    entries.push_back({QStringLiteral("META-INF/container.xml"),
                       QByteArrayLiteral("<?xml version=\"1.0\"?>"
                                         "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                                         "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
                                         "media-type=\"application/oebps-package+xml\"/></rootfiles></container>"),
                       0});
    entries.push_back({QStringLiteral("OEBPS/content.opf"), opf, 0});
    entries.push_back({QStringLiteral("OEBPS/nav.xhtml"), nav, 0});
    entries.push_back({QStringLiteral("OEBPS/toc.ncx"), ncx, 0});
    entries.push_back({QStringLiteral("OEBPS/c1.xhtml"),
                       QByteArrayLiteral("<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>甲段</p></body></html>"),
                       0});
    entries.push_back({QStringLiteral("OEBPS/c2.xhtml"),
                       QByteArrayLiteral("<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>乙段</p></body></html>"),
                       0});
    const QString path = dir.filePath(QStringLiteral("nav.epub"));
    QVERIFY(writeFile(path, storedZip(entries)));
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(opened->parsed.book.title, QStringLiteral("导航书"));
    QCOMPARE(opened->parsed.chapters.size(), 2U);
    QCOMPARE(opened->parsed.chapters[0].title, QStringLiteral("甲章"));
    QCOMPARE(opened->parsed.chapters[1].title, QStringLiteral("乙章"));
    QVERIFY(opened->chapter.utf8.contains(QStringLiteral("甲段").toUtf8()));
}

void CoreTest::epubReopenUsesUtf8Cache()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("book.epub"));
    QVERIFY(writeFile(path, epubPackage(kEpub2Opf, QByteArrayLiteral("OEBPS/toc.ncx"), kEpub2Ncx)));
    const QString dbPath = dir.filePath(QStringLiteral("moyu.db"));
    moyu::Database database(dbPath);
    QVERIFY(database.startAndWait());
    moyu::resetTranscodeCallCount();
    auto first = openIn(path, dir.path(), &database);
    QVERIFY2(static_cast<bool>(first), first ? "" : first.error().message.toUtf8().constData());
    QVERIFY(!first->cacheHit);
    QCOMPARE(moyu::transcodeCallCount(), 0ULL);
    const QString cache = first->parsed.bodyPath;
    QVERIFY(cache.endsWith(QStringLiteral(".utf8")));
    QVERIFY(cache != path);

    moyu::resetTranscodeCallCount();
    auto second = openIn(path, dir.path(), &database);
    QVERIFY2(static_cast<bool>(second), second ? "" : second.error().message.toUtf8().constData());
    QVERIFY(second->cacheHit);
    QCOMPARE(second->parsed.bodyPath, cache);
    QCOMPARE(moyu::transcodeCallCount(), 0ULL);
    QVERIFY(second->chapter.utf8.contains(QStringLiteral("甲段有粗体").toUtf8()));

    {
        QSqlDatabase wipe = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("wipe-epub"));
        wipe.setDatabaseName(dbPath);
        QVERIFY(wipe.open());
        QSqlQuery query(wipe);
        QVERIFY(query.exec(QStringLiteral("PRAGMA foreign_keys=ON")));
        QVERIFY(query.exec(QStringLiteral("DELETE FROM books")));
        query.finish();
        wipe.close();
        wipe = QSqlDatabase();
        QSqlDatabase::removeDatabase(QStringLiteral("wipe-epub"));
    }
    moyu::resetTranscodeCallCount();
    auto third = openIn(path, dir.path(), &database);
    QVERIFY2(static_cast<bool>(third), third ? "" : third.error().message.toUtf8().constData());
    QVERIFY(!third->cacheHit);
    QCOMPARE(moyu::transcodeCallCount(), 0ULL);
    QVERIFY(third->chapter.utf8.contains(QStringLiteral("甲段有粗体").toUtf8()));
    QCOMPARE(third->parsed.bodyPath, cache);
}

void CoreTest::corruptEpubDoesNotCrash()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("bad.epub"));
    QVERIFY(writeFile(path, QByteArrayLiteral("this is not a zip")));
    auto opened = openIn(path, dir.path());
    QVERIFY(!opened);
    QVERIFY(!opened.error().message.isEmpty());
}

void CoreTest::zipEntryLargerThanCapIsRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    StoredEntry huge;
    huge.name = QStringLiteral("big.bin");
    huge.bytes = QByteArrayLiteral("x");
    huge.declaredSize = static_cast<quint32>(moyu::kMaxZipEntryBytes + 1);
    const QString path = dir.filePath(QStringLiteral("huge.epub"));
    QVERIFY(writeFile(path, storedZip({huge})));
    auto opened = openIn(path, dir.path());
    QVERIFY(!opened);
    QCOMPARE(opened.error().message, QStringLiteral("压缩包里有过大的文件"));
}

void CoreTest::encryptedEpubIsRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray encryption = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<encryption xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<EncryptedData xmlns=\"http://www.w3.org/2001/04/xmlenc#\">"
        "<EncryptionMethod Algorithm=\"http://www.w3.org/2001/04/xmlenc#aes128-cbc\"/>"
        "<CipherData><CipherReference URI=\"OEBPS/c1.xhtml\"/></CipherData>"
        "</EncryptedData></encryption>");
    const QString path = dir.filePath(QStringLiteral("drm.epub"));
    QVERIFY(writeFile(path, epubPackage(kEpub2Opf, QByteArrayLiteral("META-INF/encryption.xml"), encryption)));
    auto opened = openIn(path, dir.path());
    QVERIFY(!opened);
    QCOMPARE(opened.error().message, QStringLiteral("这本书有 DRM，不能打开"));
}

void CoreTest::fontObfuscationIsNotDrm()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray encryption = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<encryption xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<EncryptedData xmlns=\"http://www.w3.org/2001/04/xmlenc#\">"
        "<EncryptionMethod Algorithm=\"http://www.idpf.org/2008/embedding\"/>"
        "<CipherData><CipherReference URI=\"OEBPS/font.otf\"/></CipherData>"
        "</EncryptedData></encryption>");
    const QString path = dir.filePath(QStringLiteral("font.epub"));
    QVERIFY(writeFile(path, epubPackage(kEpub2Opf, QByteArrayLiteral("META-INF/encryption.xml"), encryption)));
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QVERIFY(opened->chapter.utf8.contains(QStringLiteral("甲段有粗体").toUtf8()));
}

void CoreTest::epubPathTraversalIsRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray opf = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uid\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>越界</dc:title></metadata>"
        "<manifest><item id=\"c1\" href=\"../secret.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
        "<spine><itemref idref=\"c1\"/></spine></package>");
    const QString path = dir.filePath(QStringLiteral("escape.epub"));
    QVERIFY(writeFile(path, epubPackage(opf, {}, {})));
    auto opened = openIn(path, dir.path());
    QVERIFY(!opened);
    QCOMPARE(opened.error().message, QStringLiteral("没有可阅读的章节"));
}

void CoreTest::htmlHeadingsBecomeChapters()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("page.html"));
    QVERIFY(writeFile(path,
                      QStringLiteral("<html><body><h1>甲</h1><p>甲段</p><h2>乙</h2><p>乙段</p></body></html>").toUtf8()));
    moyu::resetTranscodeCallCount();
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(moyu::transcodeCallCount(), 0ULL);
    QCOMPARE(opened->parsed.book.format, moyu::BookFormat::Html);
    QCOMPARE(opened->parsed.chapters.size(), 2U);
    QCOMPARE(opened->parsed.chapters[0].title, QStringLiteral("甲"));
    QCOMPARE(opened->parsed.chapters[1].title, QStringLiteral("乙"));
    QVERIFY(opened->chapter.utf8.contains(QStringLiteral("甲段").toUtf8()));
    auto second = moyu::readMappedChapter(opened->parsed.bodyPath, 0, opened->parsed.chapters[1]);
    QVERIFY(second);
    QVERIFY(second->utf8.contains(QStringLiteral("乙段").toUtf8()));
}

void CoreTest::markdownHeadingsBecomeChapters()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("note.md"));
    QVERIFY(writeFile(path, QStringLiteral("# 甲\n甲段\n## 乙\n乙段\n").toUtf8()));
    moyu::resetTranscodeCallCount();
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(moyu::transcodeCallCount(), 0ULL);
    QCOMPARE(opened->parsed.book.format, moyu::BookFormat::Markdown);
    QCOMPARE(opened->parsed.chapters.size(), 2U);
    QCOMPARE(opened->parsed.chapters[0].title, QStringLiteral("甲"));
    QCOMPARE(opened->parsed.chapters[1].title, QStringLiteral("乙"));
    QVERIFY(opened->chapter.utf8.contains(QStringLiteral("甲段").toUtf8()));
}

void CoreTest::fb2SectionsBecomeChapters()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("book.fb2"));
    const QString xml = QStringLiteral(
        "<?xml version=\"1.0\"?>"
        "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\">"
        "<description><title-info><book-title>浮生</book-title>"
        "<author><first-name>张</first-name><last-name>三</last-name></author>"
        "</title-info></description><body>"
        "<section><title><p>第一节</p></title><p>甲段</p>"
        "<section><title><p>嵌套</p></title><p>乙段</p></section>"
        "</section></body></FictionBook>");
    QVERIFY(writeFile(path, xml.toUtf8()));
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(opened->parsed.book.format, moyu::BookFormat::Fb2);
    QCOMPARE(opened->parsed.book.title, QStringLiteral("浮生"));
    QCOMPARE(opened->parsed.book.author, QStringLiteral("张三"));
    QCOMPARE(opened->parsed.chapters.size(), 2U);
    QCOMPARE(opened->parsed.chapters[0].title, QStringLiteral("第一节"));
    QCOMPARE(opened->parsed.chapters[0].level, static_cast<quint8>(1));
    QCOMPARE(opened->parsed.chapters[1].title, QStringLiteral("嵌套"));
    QCOMPARE(opened->parsed.chapters[1].level, static_cast<quint8>(2));
    QVERIFY(opened->chapter.utf8.contains(QStringLiteral("甲段").toUtf8()));
    auto second = moyu::readMappedChapter(opened->parsed.bodyPath, 0, opened->parsed.chapters[1]);
    QVERIFY(second);
    QVERIFY(second->utf8.contains(QStringLiteral("乙段").toUtf8()));
}

void CoreTest::corruptMobiDoesNotCrash()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray garbage(256, 'x');
    for (const char* name : {"bad.mobi", "bad.azw3"}) {
        const QString path = dir.filePath(QString::fromLatin1(name));
        QVERIFY(writeFile(path, garbage));
        auto opened = openIn(path, dir.path());
        QVERIFY(!opened);
        QCOMPARE(opened.error().message, QStringLiteral("MOBI 无法解析"));
    }
}

void CoreTest::unsupportedExtensionIsRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto opened = openIn(dir.filePath(QStringLiteral("notes.pdf")), dir.path());
    QVERIFY(!opened);
    QCOMPARE(opened.error().message, QStringLiteral("不支持的文件格式"));
}

void CoreTest::deepMarkupDoesNotCrash()
{
    const QByteArray html = QByteArrayLiteral("<html><body>") + nestedTags(QByteArrayLiteral("<b>"), QByteArrayLiteral("</b>"), QByteArrayLiteral("正文"), 100000) +
                            QByteArrayLiteral("</body></html>");
    auto plain = moyu::xhtmlToPlain(html.constData(), static_cast<quint64>(html.size()));
    QVERIFY(!plain);
    QCOMPARE(plain.error().message, QStringLiteral("文件结构异常"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString htmlPath = dir.filePath(QStringLiteral("deep.html"));
    QVERIFY(writeFile(htmlPath, html));
    auto opened = openIn(htmlPath, dir.path());
    if (opened) {
        QVERIFY(opened->chapter.utf8.contains(QStringLiteral("正文").toUtf8()));
    } else {
        QCOMPARE(opened.error().message, QStringLiteral("文件结构异常"));
    }

    QByteArray fb2 = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\"><body>");
    fb2 += nestedTags(QByteArrayLiteral("<section>"), QByteArrayLiteral("</section>"), QByteArrayLiteral("<p>甲</p>"), 100000);
    fb2 += QByteArrayLiteral("</body></FictionBook>");
    const QString fb2Path = dir.filePath(QStringLiteral("deep.fb2"));
    QVERIFY(writeFile(fb2Path, fb2));
    auto fb2Opened = openIn(fb2Path, dir.path());
    if (fb2Opened) {
        QVERIFY(fb2Opened->chapter.utf8.contains(QStringLiteral("甲").toUtf8()));
    } else {
        QCOMPARE(fb2Opened.error().message, QStringLiteral("文件结构异常"));
    }
}

void CoreTest::ampersandRunFinishesQuickly()
{
    const QByteArray html(16 * 1024 * 1024, '&');
    QElapsedTimer timer;
    timer.start();
    const moyu::MarkupText text = moyu::htmlToPlainLenient(html.constData(), static_cast<quint64>(html.size()));
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(elapsed < 1000, qPrintable(QStringLiteral("解析 %1 ms").arg(elapsed)));
    QCOMPARE(text.text.size(), html.size());
}

void CoreTest::selfClosingScriptKeepsFollowingText()
{
    const QByteArray script = QByteArrayLiteral("<script/>正文");
    const moyu::MarkupText scriptText = moyu::htmlToPlainLenient(script.constData(), static_cast<quint64>(script.size()));
    QVERIFY(scriptText.text.contains(QStringLiteral("正文")));

    const QByteArray style = QByteArrayLiteral("<style />甲");
    const moyu::MarkupText styleText = moyu::htmlToPlainLenient(style.constData(), static_cast<quint64>(style.size()));
    QVERIFY(styleText.text.contains(QStringLiteral("甲")));

    const QByteArray closed = QByteArrayLiteral("<script>隐藏</script>后文");
    const moyu::MarkupText closedText = moyu::htmlToPlainLenient(closed.constData(), static_cast<quint64>(closed.size()));
    QVERIFY(closedText.text.contains(QStringLiteral("后文")));
    QVERIFY(!closedText.text.contains(QStringLiteral("隐藏")));
}

void CoreTest::legalDotDotPathOpens()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray opf = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uid\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>回退</dc:title></metadata>"
        "<manifest><item id=\"c1\" href=\"text/../c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
        "<spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>");
    const QString path = dir.filePath(QStringLiteral("dotdot.epub"));
    QVERIFY(writeFile(path, epubPackage(opf, {}, {})));
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(opened->parsed.chapters.size(), 2U);
    QVERIFY(opened->chapter.utf8.contains(QStringLiteral("甲段有粗体").toUtf8()));

    const QByteArray rootOpf = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uid\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>根</dc:title></metadata>"
        "<manifest><item id=\"s\" href=\"../secret.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
        "<spine><itemref idref=\"s\"/></spine></package>");
    const QByteArray secret = QByteArrayLiteral(
        "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>根上的字</p></body></html>");
    const QString rootPath = dir.filePath(QStringLiteral("root.epub"));
    QVERIFY(writeFile(rootPath, epubPackage(rootOpf, QByteArrayLiteral("secret.xhtml"), secret)));
    auto rooted = openIn(rootPath, dir.path());
    QVERIFY2(static_cast<bool>(rooted), rooted ? "" : rooted.error().message.toUtf8().constData());
    QVERIFY(rooted->chapter.utf8.contains(QStringLiteral("根上的字").toUtf8()));
}

void CoreTest::remoteManifestItemIsSkipped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray opf = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uid\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>远程</dc:title></metadata>"
        "<manifest>"
        "<item id=\"remote\" href=\"https://example.com/a.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"out\" href=\"../../outside.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "</manifest><spine><itemref idref=\"remote\"/><itemref idref=\"out\"/><itemref idref=\"c1\"/></spine></package>");
    const QString path = dir.filePath(QStringLiteral("remote.epub"));
    QVERIFY(writeFile(path, epubPackage(opf, {}, {})));
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(opened->parsed.chapters.size(), 1U);
    QVERIFY(opened->chapter.utf8.contains(QStringLiteral("甲段有粗体").toUtf8()));
}

void CoreTest::missingSpineItemIsSkipped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray opf = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uid\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>缺文件</dc:title></metadata>"
        "<manifest>"
        "<item id=\"gone\" href=\"gone.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "</manifest><spine><itemref idref=\"gone\"/><itemref idref=\"c1\"/></spine></package>");
    const QString path = dir.filePath(QStringLiteral("missing.epub"));
    QVERIFY(writeFile(path, epubPackage(opf, {}, {})));
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(opened->parsed.chapters.size(), 1U);
    QVERIFY(opened->chapter.utf8.contains(QStringLiteral("甲段有粗体").toUtf8()));
}

void CoreTest::fb2InlineTagsKeepText()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("inline.fb2"));
    const QString xml = QStringLiteral(
        "<?xml version=\"1.0\"?>"
        "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\">"
        "<description><title-info><book-title>浮<emphasis>生</emphasis></book-title>"
        "<author><first-name>张<emphasis>三</emphasis></first-name></author>"
        "</title-info></description><body>"
        "<section><title><p>节</p></title><p>他说<emphasis>不</emphasis>行</p></section>"
        "</body></FictionBook>");
    QVERIFY(writeFile(path, xml.toUtf8()));
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(opened->parsed.book.title, QStringLiteral("浮生"));
    QCOMPARE(opened->parsed.book.author, QStringLiteral("张三"));
    QVERIFY(opened->chapter.utf8.contains(QStringLiteral("他说不行").toUtf8()));
}

bool hasNamedEntities(const QString& text)
{
    return text.contains(QChar(0x00A0)) && text.contains(QChar(0x2014)) && text.contains(QChar(0x2013)) &&
           text.contains(QChar(0x201C)) && text.contains(QChar(0x201D)) && text.contains(QChar(0x2018)) &&
           text.contains(QChar(0x2019)) && text.contains(QChar(0x2026)) && text.contains(QChar(0x00B7)) &&
           text.contains(QChar(0x00A9)) && text.contains(QStringLiteral("甲")) && text.contains(QStringLiteral("乙"));
}

void CoreTest::namedEntitiesDecodeOnBothPaths()
{
    const QByteArray body = QByteArrayLiteral("甲&nbsp;&mdash;&ndash;&ldquo;&rdquo;&lsquo;&rsquo;&hellip;&middot;&copy;乙");
    const QByteArray xhtml = QByteArrayLiteral("<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>") + body +
                             QByteArrayLiteral("</p></body></html>");
    auto parsed = moyu::xhtmlToPlain(xhtml.constData(), static_cast<quint64>(xhtml.size()));
    QVERIFY2(static_cast<bool>(parsed), parsed ? "" : parsed.error().message.toUtf8().constData());
    QVERIFY(hasNamedEntities(parsed->text));

    const QByteArray broken = QByteArrayLiteral("<p>") + body;
    const moyu::MarkupText lenient = moyu::htmlToPlainLenient(broken.constData(), static_cast<quint64>(broken.size()));
    QVERIFY(hasNamedEntities(lenient.text));
}

void CoreTest::epubFragmentSplitsChapters()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray opf = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"uid\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>锚点</dc:title><dc:language>zh</dc:language><dc:identifier id=\"uid\">id</dc:identifier>"
        "</metadata><manifest>"
        "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>"
        "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "</manifest><spine><itemref idref=\"c1\"/></spine></package>");
    const QByteArray nav = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><body>"
        "<nav epub:type=\"toc\"><ol><li><a href=\"c1.xhtml\">前言</a><ol>"
        "<li><a href=\"c1.xhtml#a\">甲</a></li>"
        "<li><a href=\"c1.xhtml#b\">乙</a></li>"
        "</ol></li></ol></nav></body></html>");
    const QByteArray chapter = QByteArrayLiteral(
        "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
        "<p>前言段</p><h2 id=\"a\">锚甲</h2><p>甲段</p><p id=\"b\">乙段</p>"
        "</body></html>");
    QList<StoredEntry> entries;
    entries.push_back({QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"), 0});
    entries.push_back({QStringLiteral("META-INF/container.xml"),
                       QByteArrayLiteral("<?xml version=\"1.0\"?>"
                                         "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                                         "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
                                         "media-type=\"application/oebps-package+xml\"/></rootfiles></container>"),
                       0});
    entries.push_back({QStringLiteral("OEBPS/content.opf"), opf, 0});
    entries.push_back({QStringLiteral("OEBPS/nav.xhtml"), nav, 0});
    entries.push_back({QStringLiteral("OEBPS/c1.xhtml"), chapter, 0});
    const QString path = dir.filePath(QStringLiteral("frag.epub"));
    QVERIFY(writeFile(path, storedZip(entries)));
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(opened->parsed.chapters.size(), 3U);
    QCOMPARE(opened->parsed.chapters[0].title, QStringLiteral("前言"));
    QCOMPARE(opened->parsed.chapters[0].level, static_cast<quint8>(1));
    QCOMPARE(opened->parsed.chapters[1].title, QStringLiteral("甲"));
    QCOMPARE(opened->parsed.chapters[1].level, static_cast<quint8>(2));
    QCOMPARE(opened->parsed.chapters[2].title, QStringLiteral("乙"));
    QCOMPARE(opened->parsed.chapters[2].level, static_cast<quint8>(2));
    QVERIFY(opened->chapter.utf8.contains(QStringLiteral("前言段").toUtf8()));
    auto second = moyu::readMappedChapter(opened->parsed.bodyPath, 0, opened->parsed.chapters[1]);
    QVERIFY(second);
    QVERIFY(second->utf8.contains(QStringLiteral("甲段").toUtf8()));
    auto third = moyu::readMappedChapter(opened->parsed.bodyPath, 0, opened->parsed.chapters[2]);
    QVERIFY(third);
    QVERIFY(third->utf8.contains(QStringLiteral("乙段").toUtf8()));

    const QByteArray ncxOpf = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uid\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>层级</dc:title></metadata>"
        "<manifest>"
        "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
        "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "</manifest><spine toc=\"ncx\"><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine></package>");
    const QByteArray ncx = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><navMap>"
        "<navPoint id=\"v\"><navLabel><text>卷一</text></navLabel><content src=\"c1.xhtml\"/>"
        "<navPoint id=\"c\"><navLabel><text>第一章</text></navLabel><content src=\"c2.xhtml\"/></navPoint>"
        "</navPoint></navMap></ncx>");
    const QString ncxPath = dir.filePath(QStringLiteral("levels.epub"));
    QVERIFY(writeFile(ncxPath, epubPackage(ncxOpf, QByteArrayLiteral("OEBPS/toc.ncx"), ncx)));
    auto leveled = openIn(ncxPath, dir.path());
    QVERIFY2(static_cast<bool>(leveled), leveled ? "" : leveled.error().message.toUtf8().constData());
    QCOMPARE(leveled->parsed.chapters.size(), 2U);
    QCOMPARE(leveled->parsed.chapters[0].title, QStringLiteral("卷一"));
    QCOMPARE(leveled->parsed.chapters[0].level, static_cast<quint8>(1));
    QCOMPARE(leveled->parsed.chapters[1].title, QStringLiteral("第一章"));
    QCOMPARE(leveled->parsed.chapters[1].level, static_cast<quint8>(2));
}

void CoreTest::markdownFenceIsNotAChapter()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("fenced.md"));
    const QString markdown = QStringLiteral("# 真章\n甲\n```\n# 假章\n乙\n```\n# 真后\n丙\n~~~\n# 也是假的\n~~~\n");
    QVERIFY(writeFile(path, markdown.toUtf8()));
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(opened->parsed.chapters.size(), 2U);
    QCOMPARE(opened->parsed.chapters[0].title, QStringLiteral("真章"));
    QCOMPARE(opened->parsed.chapters[1].title, QStringLiteral("真后"));
    QVERIFY(opened->chapter.utf8.contains(QStringLiteral("假章").toUtf8()));
    auto second = moyu::readMappedChapter(opened->parsed.bodyPath, 0, opened->parsed.chapters[1]);
    QVERIFY(second);
    QVERIFY(second->utf8.contains(QStringLiteral("丙").toUtf8()));
    QVERIFY(second->utf8.contains(QStringLiteral("也是假的").toUtf8()));
}

void CoreTest::htmlMetaCharsetWins()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QByteArray gbk = QByteArrayLiteral("<html><head><meta charset=\"gbk\"></head><body><p>");
    gbk.append(static_cast<char>(0xB5));
    gbk.append(static_cast<char>(0xDA));
    gbk.append('1');
    gbk.append(static_cast<char>(0xD5));
    gbk.append(static_cast<char>(0xC2));
    gbk.append(' ');
    gbk.append(static_cast<char>(0xBC));
    gbk.append(static_cast<char>(0xD7));
    gbk.append(QByteArrayLiteral("</p></body></html>"));
    const QString gbkPath = dir.filePath(QStringLiteral("gbk.html"));
    QVERIFY(writeFile(gbkPath, gbk));
    auto opened = openIn(gbkPath, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QVERIFY(opened->chapter.utf8.contains(QStringLiteral("第1章 甲").toUtf8()));

    QByteArray httpEquiv = QByteArrayLiteral("<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=gbk\"></head><body><p>");
    httpEquiv.append(gbk.mid(gbk.indexOf("<p>") + 3));
    const QString equivPath = dir.filePath(QStringLiteral("equiv.html"));
    QVERIFY(writeFile(equivPath, httpEquiv));
    auto equiv = openIn(equivPath, dir.path());
    QVERIFY2(static_cast<bool>(equiv), equiv ? "" : equiv.error().message.toUtf8().constData());
    QVERIFY(equiv->chapter.utf8.contains(QStringLiteral("第1章 甲").toUtf8()));

    moyu::resetTranscodeCallCount();
    const QString utf8Path = dir.filePath(QStringLiteral("utf8.html"));
    QVERIFY(writeFile(utf8Path, QStringLiteral("<html><head><meta charset=\"utf-8\"></head><body><p>甲</p></body></html>").toUtf8()));
    auto utf8 = openIn(utf8Path, dir.path());
    QVERIFY2(static_cast<bool>(utf8), utf8 ? "" : utf8.error().message.toUtf8().constData());
    QCOMPARE(moyu::transcodeCallCount(), 0ULL);
    QVERIFY(utf8->chapter.utf8.contains(QStringLiteral("甲").toUtf8()));
}

void CoreTest::encryptionWithoutMethodIsDrm()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray bare = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<encryption xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<EncryptedData xmlns=\"http://www.w3.org/2001/04/xmlenc#\">"
        "<CipherData><CipherReference URI=\"OEBPS/c1.xhtml\"/></CipherData>"
        "</EncryptedData></encryption>");
    const QString barePath = dir.filePath(QStringLiteral("bare.epub"));
    QVERIFY(writeFile(barePath, epubPackage(kEpub2Opf, QByteArrayLiteral("META-INF/encryption.xml"), bare)));
    auto bareOpened = openIn(barePath, dir.path());
    QVERIFY(!bareOpened);
    QCOMPARE(bareOpened.error().message, QStringLiteral("这本书有 DRM，不能打开"));

    const QByteArray mixed = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<encryption xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<EncryptedData xmlns=\"http://www.w3.org/2001/04/xmlenc#\">"
        "<EncryptionMethod Algorithm=\"http://www.idpf.org/2008/embedding\"/>"
        "<CipherData><CipherReference URI=\"OEBPS/font.otf\"/></CipherData>"
        "</EncryptedData>"
        "<EncryptedData xmlns=\"http://www.w3.org/2001/04/xmlenc#\">"
        "<CipherData><CipherReference URI=\"OEBPS/c1.xhtml\"/></CipherData>"
        "</EncryptedData></encryption>");
    const QString mixedPath = dir.filePath(QStringLiteral("mixed.epub"));
    QVERIFY(writeFile(mixedPath, epubPackage(kEpub2Opf, QByteArrayLiteral("META-INF/encryption.xml"), mixed)));
    auto mixedOpened = openIn(mixedPath, dir.path());
    QVERIFY(!mixedOpened);
    QCOMPARE(mixedOpened.error().message, QStringLiteral("这本书有 DRM，不能打开"));
}

void CoreTest::deflateBombIsRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray zip = deflatedZip(QStringLiteral("bomb.bin"), moyu::kMaxZipEntryBytes + (1024ULL * 1024ULL), 32);
    QVERIFY(!zip.isEmpty());
    const QString path = dir.filePath(QStringLiteral("bomb.zip"));
    QVERIFY(writeFile(path, zip));
    auto opened = moyu::ZipArchive::open(path);
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    auto bytes = opened->read(QStringLiteral("bomb.bin"));
    QVERIFY(!bytes);
    QCOMPARE(bytes.error().message, QStringLiteral("压缩包展开后太大"));
}

void CoreTest::truncatedCentralDirectoryDoesNotCrash()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QByteArray zip = storedZip({{QStringLiteral("a.txt"), QByteArrayLiteral("hello"), 0}});
    const qsizetype directory = zip.indexOf(QByteArray("\x50\x4b\x01\x02", 4));
    QVERIFY(directory > 30);
    // 中央目录写到一半就截断。库可能会从本地文件头恢复，但不能崩。
    zip.truncate(directory + 8);
    const QString path = dir.filePath(QStringLiteral("cut.zip"));
    QVERIFY(writeFile(path, zip));
    auto opened = moyu::ZipArchive::open(path);
    if (!opened) {
        QCOMPARE(opened.error().message, QStringLiteral("无法打开压缩包"));
    } else {
        auto bytes = opened->read(QStringLiteral("a.txt"));
        if (!bytes) {
            QVERIFY(!bytes.error().message.isEmpty());
        }
    }
}

void CoreTest::encryptedMobiHeaderIsRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("locked.mobi"));
    QVERIFY(writeFile(path, palmMobi(QByteArrayLiteral("secret"), 2)));
    auto opened = openIn(path, dir.path());
    QVERIFY(!opened);
    QCOMPARE(opened.error().message, QStringLiteral("这本书有 DRM，不能打开"));
}

void CoreTest::singleMobiPartCanExceedThirtyTwoMegabytes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray marker = QStringLiteral("MOBI大书\n").toUtf8();
    QByteArray text(static_cast<qsizetype>(32ULL * 1024ULL * 1024ULL + 4096ULL), 'x');
    std::memcpy(text.data(), marker.constData(), static_cast<std::size_t>(marker.size()));
    const QString path = dir.filePath(QStringLiteral("long.mobi"));
    QVERIFY(writeFile(path, palmMobi(text, 0)));
    auto opened = openIn(path, dir.path());
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QVERIFY(opened->parsed.book.totalBytes > 32ULL * 1024ULL * 1024ULL);
    // Palm DOC 没有编码记录，非 ASCII 会被按单字节处理。标记用纯 ASCII。
    QVERIFY(opened->chapter.utf8.contains("MOBI"));
    QVERIFY(opened->chapter.utf8.contains('x'));
}

void CoreTest::customRegexBecomesChapters()
{
    const QByteArray text = QStringLiteral("前文\n第1章 旧\n【甲】\n甲段\n【乙】\n乙段\n").toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(text.constData(), static_cast<quint64>(text.size()), &cancel, QStringLiteral("^【[^】]+】"));
    QVERIFY2(static_cast<bool>(chapters), chapters ? "" : chapters.error().message.toUtf8().constData());
    QCOMPARE(chapters->size(), 3U);
    QCOMPARE(chapters->at(0).title, QStringLiteral("开头"));
    QCOMPARE(chapters->at(1).title, QStringLiteral("【甲】"));
    QCOMPARE(chapters->at(2).title, QStringLiteral("【乙】"));
    QCOMPARE(chapters->at(1).level, static_cast<quint8>(2));
}

void CoreTest::customRegexIgnoresLongLines()
{
    const QString longLine(41, QChar(u'甲'));
    const QByteArray text = (QStringLiteral("前文\n") + longLine + QStringLiteral("\n章\n后\n")).toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(text.constData(), static_cast<quint64>(text.size()), &cancel, QStringLiteral(".+"));
    QVERIFY2(static_cast<bool>(chapters), chapters ? "" : chapters.error().message.toUtf8().constData());
    QCOMPARE(chapters->size(), 3U);
    for (const moyu::ChapterIndex& chapter : *chapters) {
        QVERIFY(chapter.title.size() <= 40);
        QVERIFY(chapter.title != longLine);
    }
    QCOMPARE(chapters->at(0).title, QStringLiteral("前文"));
    QCOMPARE(chapters->at(1).title, QStringLiteral("章"));
    QCOMPARE(chapters->at(2).title, QStringLiteral("后"));
}

void CoreTest::invalidChapterRegexIsAnError()
{
    const QByteArray text = QStringLiteral("第1章 甲\n正文\n").toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(text.constData(), static_cast<quint64>(text.size()), &cancel, QStringLiteral("("));
    QVERIFY(!chapters);
    QCOMPARE(chapters.error().message, QStringLiteral("章节正则无效"));
}

void CoreTest::customRegexFallsBackToBuiltin()
{
    const QByteArray text = QStringLiteral("第1章 甲\n正文\n").toUtf8();
    std::atomic<bool> cancel{false};
    auto chapters = moyu::detectChapters(text.constData(), static_cast<quint64>(text.size()), &cancel, QStringLiteral("^【】"));
    QVERIFY(chapters);
    QCOMPARE(chapters->size(), 1U);
    QCOMPARE(chapters->front().title, QStringLiteral("第1章 甲"));
}

void CoreTest::invalidRegexStillOpensTxt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("a.txt"));
    QVERIFY(writeFile(path, QStringLiteral("第1章 甲\n正文\n").toUtf8()));
    std::atomic<bool> cancel{false};
    auto opened = moyu::openTxtFile(path, dir.path(), nullptr, 0, 0, cancel, {}, nullptr, QStringLiteral("("));
    QVERIFY2(static_cast<bool>(opened), opened ? "" : opened.error().message.toUtf8().constData());
    QCOMPARE(opened->parsed.chapters.size(), 1U);
    QCOMPARE(opened->parsed.chapters.front().title, QStringLiteral("第1章 甲"));
}

void CoreTest::shelfListsNewestFirst()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    moyu::Database database(dir.filePath(QStringLiteral("moyu.db")));
    QVERIFY(database.startAndWait());

    auto insert = [&](const QString& title, const QString& token, qint64 when, quint64 total) {
        moyu::Book book;
        book.filePath = dir.filePath(title + QStringLiteral(".txt"));
        book.fingerprint = token;
        book.title = title;
        book.format = moyu::BookFormat::Txt;
        book.sourceEncoding = QStringLiteral("UTF-8");
        book.totalBytes = total;
        book.lastReadAt = when;
        return database.upsertBook(book);
    };
    auto oldId = insert(QStringLiteral("旧"), QStringLiteral("old"), 1, 100);
    auto newId = insert(QStringLiteral("新"), QStringLiteral("new"), 5, 200);
    auto midId = insert(QStringLiteral("中"), QStringLiteral("mid"), 3, 50);
    QVERIFY(oldId);
    QVERIFY(newId);
    QVERIFY(midId);

    moyu::ChapterIndex chapter;
    chapter.index = 0;
    chapter.title = QStringLiteral("正文");
    chapter.level = 2;
    chapter.byteStart = 0;
    chapter.byteEnd = 100;
    QVERIFY(database.replaceChapters(*oldId, {chapter}));
    moyu::ReadingProgress progress;
    progress.bookId = *oldId;
    progress.chapterIndex = 0;
    progress.byteOffsetInChapter = 25;
    progress.updatedAt = 2;
    QVERIFY(database.saveProgress(progress));

    auto rows = database.listShelf();
    QVERIFY2(static_cast<bool>(rows), rows ? "" : rows.error().message.toUtf8().constData());
    QCOMPARE(rows->size(), 3U);
    QCOMPARE(rows->at(0).title, QStringLiteral("新"));
    QCOMPARE(rows->at(0).percent, 0);
    QCOMPARE(rows->at(1).title, QStringLiteral("中"));
    QCOMPARE(rows->at(2).title, QStringLiteral("旧"));
    QCOMPARE(rows->at(2).percent, 25);
}

void CoreTest::deleteBookRemovesRecordAndCache()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    moyu::Database database(dir.filePath(QStringLiteral("moyu.db")));
    QVERIFY(database.startAndWait());
    const QString source = dir.filePath(QStringLiteral("novel.txt"));
    QVERIFY(writeFile(source, QByteArrayLiteral("hello")));
    const QString cacheDir = dir.filePath(QStringLiteral("cache"));
    QVERIFY(QDir().mkpath(cacheDir));
    const QString token = QStringLiteral("token1");
    QVERIFY(writeFile(moyu::utf8CachePath(cacheDir, token), QByteArrayLiteral("cached")));

    moyu::Book book;
    book.filePath = source;
    book.fingerprint = token;
    book.title = QStringLiteral("甲");
    book.format = moyu::BookFormat::Txt;
    book.sourceEncoding = QStringLiteral("UTF-8");
    book.totalBytes = 5;
    book.lastReadAt = 10;
    auto id = database.upsertBook(book);
    QVERIFY(id);
    moyu::Bookmark bookmark;
    bookmark.bookId = *id;
    bookmark.preview = QStringLiteral("记");
    bookmark.createdAt = 1;
    QVERIFY(database.addBookmark(bookmark));

    QVERIFY(database.deleteBook(*id, cacheDir));
    QVERIFY(QFileInfo::exists(source));
    QVERIFY(!QFileInfo::exists(moyu::utf8CachePath(cacheDir, token)));
    auto rows = database.listShelf();
    QVERIFY(rows);
    QVERIFY(rows->empty());
    auto marks = database.bookmarks(*id);
    QVERIFY(marks);
    QVERIFY(marks->empty());
}

void CoreTest::retargetKeepsProgress()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    moyu::Database database(dir.filePath(QStringLiteral("moyu.db")));
    QVERIFY(database.startAndWait());
    const QString oldPath = dir.filePath(QStringLiteral("old.txt"));
    const QString newPath = dir.filePath(QStringLiteral("new.txt"));
    moyu::Book book;
    book.filePath = oldPath;
    book.fingerprint = QStringLiteral("same");
    book.title = QStringLiteral("甲");
    book.format = moyu::BookFormat::Txt;
    book.sourceEncoding = QStringLiteral("UTF-8");
    book.totalBytes = 20;
    book.lastReadAt = 4;
    auto id = database.upsertBook(book);
    QVERIFY(id);
    moyu::ReadingProgress progress;
    progress.bookId = *id;
    progress.chapterIndex = 1;
    progress.byteOffsetInChapter = 8;
    progress.updatedAt = 9;
    QVERIFY(database.saveProgress(progress));

    QVERIFY(database.retargetBook(*id, newPath));
    auto moved = database.findByPath(newPath);
    QVERIFY(moved);
    QVERIFY(moved->has_value());
    QCOMPARE((*moved)->book.id, *id);
    QVERIFY((*moved)->progress.has_value());
    QCOMPARE((*moved)->progress->chapterIndex, 1U);
    QCOMPARE((*moved)->progress->byteOffsetInChapter, 8U);
    auto stale = database.findByPath(oldPath);
    QVERIFY(stale);
    QVERIFY(!stale->has_value());
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    CoreTest tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_core.moc"
