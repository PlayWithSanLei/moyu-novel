package app.moyureader.core

import org.junit.jupiter.api.Assertions.assertArrayEquals
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertNotNull
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.io.TempDir
import java.io.File
import java.nio.file.Path
import java.util.concurrent.atomic.AtomicBoolean

class CorePortTest {
    @TempDir
    lateinit var temp: Path

    @Test
    fun utf16ChunkDoesNotSplitShang() {
        val data = bytes(0x0A, 0x4E, 0x0A, 0x00, 0x7D, 0x59)
        assertEquals(4L, findChunkEnd(data, 1, NewlineCut.Utf16Le))
        assertEquals(4L, findChunkEnd(data, 2, NewlineCut.Utf16Le))
        assertEquals(4L, findChunkEnd(data, 100, NewlineCut.Utf16Le))
    }

    @Test
    fun gbkChunkKeepsCharacterBeforeNewline() {
        val data = bytes(0xC4, 0xE3, 0x0A, 0xBA, 0xC3)
        assertEquals(3L, findChunkEnd(data, 1, NewlineCut.Bytes))
    }

    @Test
    fun transcodeKnownSamples() {
        val hello = transcode(bytes(0xC4, 0xE3, 0xBA, 0xC3), encoding("GB18030", "GB18030", NewlineCut.Bytes))
        assertArrayEquals(bytes(0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD), hello)

        val euro = transcode(bytes(0xC4, 0xE3, 0xBA, 0xC3, 0xA2, 0xE3), encoding("GB18030", "GB18030", NewlineCut.Bytes))
        assertArrayEquals(bytes(0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD, 0xE2, 0x82, 0xAC), euro)

        val big5 = transcode(bytes(0xB4, 0xFA, 0xB8, 0xD5), encoding("BIG5", "BIG5", NewlineCut.Bytes))
        assertArrayEquals(bytes(0xE6, 0xB8, 0xAC, 0xE8, 0xA9, 0xA6), big5)

        val utf16 = transcode(bytes(0x0A, 0x4E, 0x0A, 0x00, 0x7D, 0x59), encoding("UTF-16LE", "UTF-16LE", NewlineCut.Utf16Le))
        assertArrayEquals(bytes(0xE4, 0xB8, 0x8A, 0x0A, 0xE5, 0xA5, 0xBD), utf16)
    }

    @Test
    fun invalidByteBecomesReplacement() {
        val output = transcode(bytes(0xFF), encoding("GB18030", "GB18030", NewlineCut.Bytes), chunkTarget = 64)
        assertArrayEquals(bytes(0xEF, 0xBF, 0xBD), output)
    }

    @Test
    fun emptyFileIsAnError() {
        val path = temp.resolve("empty.txt").toFile()
        path.writeBytes(ByteArray(0))
        val opened = importTxt(path.absolutePath, temp.toString())
        assertFalse(opened.isSuccess)
        assertEquals("文件是空的", opened.errorOrNull()?.message)
    }

    @Test
    fun utf8BomSkipsHeader() {
        val data = bytes(0xEF, 0xBB, 0xBF, 0xE4, 0xBD, 0xA0)
        val detected = detectEncoding(data).valueOrNull()
        assertNotNull(detected)
        assertEquals("UTF-8", detected!!.canonical)
        assertEquals(3L, detected.bodyOffset)
        assertTrue(detected.passthroughUtf8())
    }

    @Test
    fun chineseAndVolumeLevels() {
        val text = "第一卷 起源\n正文\n第1章 甲\n更多\n".toByteArray()
        val chapters = chaptersOf(text)
        assertEquals(2, chapters.size)
        assertEquals("第一卷 起源", chapters[0].title)
        assertEquals(1, chapters[0].level)
        assertEquals("第1章 甲", chapters[1].title)
        assertEquals(2, chapters[1].level)
        assertTrue(chapterRangesValid(chapters, text.size.toLong()))
    }

    @Test
    fun englishAndAuxiliaryHeadings() {
        val english = "Chapter 1\nhello\nCHAPTER IV\nworld\n".toByteArray()
        val chapters = chaptersOf(english)
        assertEquals(2, chapters.size)
        assertEquals("Chapter 1", chapters[0].title)
        assertEquals("CHAPTER IV", chapters[1].title)

        val aux = "序章\n正文一段\n楔子 开始\n后文\n".toByteArray()
        val auxChapters = chaptersOf(aux)
        assertEquals(2, auxChapters.size)
        assertEquals("序章", auxChapters[0].title)
        assertEquals("楔子 开始", auxChapters[1].title)
        assertEquals(2, auxChapters[0].level)
    }

    @Test
    fun longLineIsNotAHeading() {
        val longLine = "第1章 " + "字".repeat(41)
        val text = (longLine + "\n第2章 短\n正文\n").toByteArray()
        val chapters = chaptersOf(text)
        assertEquals(2, chapters.size)
        assertEquals("开头", chapters[0].title)
        assertEquals("第2章 短", chapters[1].title)
    }

    @Test
    fun denseHeadingsCollapse() {
        val text = "第1章\n".repeat(10).toByteArray()
        val chapters = chaptersOf(text)
        assertEquals(1, chapters.size)
        assertEquals("正文", chapters[0].title)
    }

    @Test
    fun whitespacePreambleIsAbsorbed() {
        val text = "\n\n第1章 甲\n正文\n".toByteArray()
        val chapters = chaptersOf(text)
        assertEquals(1, chapters.size)
        assertEquals(0L, chapters.first().byteStart)
        assertEquals("第1章 甲", chapters.first().title)
    }

    @Test
    fun noHeadingsThenSyntheticSplit() {
        val text = "a".repeat(100_001).toByteArray()
        val chapters = chaptersOf(text)
        assertTrue(chapters.size > 1)
        assertEquals("正文", chapters.first().title)
        assertFalse(chapters.first().synthetic)
        assertEquals("正文 · 2", chapters[1].title)
        assertTrue(chapters[1].synthetic)
        assertTrue(chapterRangesValid(chapters, text.size.toLong()))
    }

    @Test
    fun rangesCoverTheBuffer() {
        val text = "前文\n第1章 甲\n中间\n第2章 乙\n结尾\n".toByteArray()
        val chapters = chaptersOf(text)
        assertTrue(chapterRangesValid(chapters, text.size.toLong()))
        assertEquals("开头", chapters.first().title)
    }

    @Test
    fun fingerprintIsStable() {
        val path = temp.resolve("a.txt").toFile()
        path.writeBytes("fingerprint-sample".toByteArray())
        val first = fingerprintOf(path).valueOrNull()
        val second = fingerprintOf(path).valueOrNull()
        assertNotNull(first)
        assertEquals(first!!.token(), second!!.token())
        assertFalse(first.token().contains(':'))
    }

    @Test
    fun asciiPrefixThenGbkIsTranscoded() {
        val data = ArrayList<Byte>()
        val line = "https://example.com/novel/chapter\n".toByteArray()
        while (data.size < 70 * 1024) {
            line.forEach { data.add(it) }
        }
        bytes(0xC4, 0xE3, 0xBA, 0xC3, 0x0A).forEach { data.add(it) }
        val path = temp.resolve("mixed.txt").toFile()
        path.writeBytes(data.toByteArray())
        val opened = importTxt(path.absolutePath, temp.toString())
        assertTrue(opened.isSuccess) { opened.errorOrNull()?.message.orEmpty() }
        val book = opened.valueOrNull()!!
        assertEquals("GB18030", book.book.sourceEncoding)
        val chapter = readChapter(book, 0).valueOrNull()!!
        assertTrue(chapter.utf8.toString(Charsets.UTF_8).contains("你好"))
        assertFalse(chapter.utf8.toList().containsSequence(bytes(0xEF, 0xBF, 0xBD).toList()))
        val cache = utf8CacheFile(temp.toFile(), book.book.fingerprint)
        assertTrue(cache.isFile)
        assertTrue(cache.readBytes().toString(Charsets.UTF_8).contains("你好"))
    }

    @Test
    fun chunkWithoutNewlineIsCapped() {
        val data = ByteArray(2 * 1024 * 1024) { 'A'.code.toByte() }
        val cut = findChunkEnd(data, 1024, NewlineCut.Bytes)
        assertTrue(cut <= 1024L * 1024)
        assertTrue(cut < data.size.toLong())
    }

    @Test
    fun crOnlyNewlinesSplitChapters() {
        val text = "第1章 甲\r正文\r第2章 乙\r世界\r".toByteArray()
        val chapters = chaptersOf(text)
        assertEquals(2, chapters.size)
        assertEquals("第1章 甲", chapters[0].title)
        assertEquals("第2章 乙", chapters[1].title)
    }

    @Test
    fun overlongParagraphIsSplit() {
        val text = "文".repeat(5000).toByteArray()
        val paragraphs = splitParagraphs(text)
        assertTrue(paragraphs.size > 1)
        for (paragraph in paragraphs) {
            assertTrue(paragraph.codePoints <= 2000)
            assertTrue(paragraph.codePoints > 0)
        }
    }

    @Test
    fun auxiliaryHeadingsMergeIntoChineseRule() {
        val text = (
            "楔子\n引文\n" +
                "第一章 甲\n正文甲\n" +
                "第二章 乙\n正文乙\n" +
                "第三章 丙\n正文丙\n" +
                "后记\n完\n" +
                "番外\n外一章\n"
            ).toByteArray()
        val titles = chaptersOf(text).map { it.title }
        assertEquals(listOf("楔子", "第一章 甲", "第二章 乙", "第三章 丙", "后记", "番外"), titles)
    }

    @Test
    fun splitVolumePiecesDropToLevelTwo() {
        val text = ("第一卷 长卷\n" + "文".repeat(120_000)).toByteArray()
        val chapters = chaptersOf(text)
        assertTrue(chapters.size > 2)
        assertEquals(1, chapters.first().level)
        assertFalse(chapters.first().synthetic)
        assertEquals(2, chapters[1].level)
        assertTrue(chapters[1].synthetic)
        assertEquals(2, chapters.last().level)
    }

    @Test
    fun big5HkscsDecodesExtension() {
        val data = ArrayList<Byte>()
        val pair = bytes(0xA7, 0x41, 0xA6, 0x6E)
        repeat(4000) { pair.forEach { data.add(it) } }
        bytes(0xA3, 0xE1).forEach { data.add(it) }
        val raw = data.toByteArray()
        val detected = detectEncoding(raw).valueOrNull()
        assertNotNull(detected)
        assertEquals("BIG5", detected!!.canonical)
        assertEquals("BIG5-HKSCS", detected.iconvName)
        val output = transcode(raw, detected, chunkTarget = 1024)
        val text = output.toString(Charsets.UTF_8)
        assertTrue(text.contains("你好"))
        assertTrue(output.toList().containsSequence(bytes(0xE2, 0x82, 0xAC).toList()))
        assertFalse(output.toList().containsSequence(bytes(0xEF, 0xBF, 0xBD).toList()))
    }

    @Test
    fun utf8CacheSkipsTranscodeWhenFileRemains() {
        val gbk = ArrayList<Byte>()
        bytes(0xB5, 0xDA, '1'.code, 0xD5, 0xC2, ' '.code, 0xBC, 0xD7, 0x0A).forEach { gbk.add(it) }
        val line = bytes(0xC4, 0xE3, 0xBA, 0xC3, 0x0A)
        while (gbk.size < 8192) {
            line.forEach { gbk.add(it) }
        }
        val path = temp.resolve("gbk.txt").toFile()
        path.writeBytes(gbk.toByteArray())
        resetTranscodeCallCount()
        val first = importTxt(path.absolutePath, temp.toString())
        assertTrue(first.isSuccess) { first.errorOrNull()?.message.orEmpty() }
        assertEquals(1L, transcodeCallCount())
        val imported = first.valueOrNull()!!
        assertTrue(imported.chapters.isNotEmpty())
        val cache = utf8CacheFile(temp.toFile(), imported.book.fingerprint)
        val cacheBytes = cache.length()
        assertTrue(cacheBytes > 0)
        val chapterCount = imported.chapters.size

        resetTranscodeCallCount()
        val second = importTxt(path.absolutePath, temp.toString())
        assertTrue(second.isSuccess) { second.errorOrNull()?.message.orEmpty() }
        assertEquals(0L, transcodeCallCount())
        assertEquals(chapterCount, second.valueOrNull()!!.chapters.size)
        assertTrue(readChapter(second.valueOrNull()!!, 0).valueOrNull()!!.utf8.toString(Charsets.UTF_8).contains("你好"))
        assertEquals(cacheBytes, cache.length())
    }

    @Test
    fun headingMarkIsNotShown() {
        val utf8 = byteArrayOf(HEADING_MARK) + "标题\n".toByteArray() + byteArrayOf(HEADING_MARK, '\n'.code.toByte()) + "正文".toByteArray()
        val paragraphs = splitParagraphs(utf8)
        assertEquals(2, paragraphs.size)
        assertEquals(ParagraphKind.Heading, paragraphs[0].kind)
        assertEquals(ParagraphKind.Body, paragraphs[1].kind)
        assertEquals("标题", paragraphText(utf8, paragraphs[0]))
        assertEquals("正文", paragraphText(utf8, paragraphs[1]))
        assertFalse(paragraphText(utf8, paragraphs[0]).contains(HEADING_MARK.toInt().toChar()))
    }

    @Test
    fun customRegexBecomesChapters() {
        val text = "前文\n第1章 旧\n【甲】\n甲段\n【乙】\n乙段\n".toByteArray()
        val chapters = chaptersOf(text, "^【[^】]+】")
        assertEquals(3, chapters.size)
        assertEquals("开头", chapters[0].title)
        assertEquals("【甲】", chapters[1].title)
        assertEquals("【乙】", chapters[2].title)
        assertEquals(2, chapters[1].level)
    }

    @Test
    fun customRegexIgnoresLongLines() {
        val longLine = "甲".repeat(41)
        val text = ("前文\n" + longLine + "\n章\n后\n").toByteArray()
        val chapters = chaptersOf(text, ".+")
        assertEquals(3, chapters.size)
        for (chapter in chapters) {
            assertTrue(chapter.title.length <= 40)
            assertTrue(chapter.title != longLine)
        }
        assertEquals("前文", chapters[0].title)
        assertEquals("章", chapters[1].title)
        assertEquals("后", chapters[2].title)
    }

    @Test
    fun invalidChapterRegexIsAnError() {
        val text = "第1章 甲\n正文\n".toByteArray()
        val chapters = detectChapters(text, AtomicBoolean(false), "(")
        assertFalse(chapters.isSuccess)
        assertEquals("章节正则无效", chapters.errorOrNull()?.message)
    }

    @Test
    fun customRegexFallsBackToBuiltin() {
        val text = "第1章 甲\n正文\n".toByteArray()
        val chapters = chaptersOf(text, "^【】")
        assertEquals(1, chapters.size)
        assertEquals("第1章 甲", chapters.first().title)
    }

    @Test
    fun invalidRegexStillOpensTxt() {
        val path = temp.resolve("a.txt").toFile()
        path.writeText("第1章 甲\n正文\n")
        val opened = importTxt(path.absolutePath, temp.toString(), chapterPattern = "(")
        assertTrue(opened.isSuccess) { opened.errorOrNull()?.message.orEmpty() }
        assertEquals(1, opened.valueOrNull()!!.chapters.size)
        assertEquals("第1章 甲", opened.valueOrNull()!!.chapters.first().title)
    }

    @Test
    fun segmentsSkipBlankAndSymbolLines() {
        val content = chapterFrom(0, "第1章 开始\n\n他走进来。\n* * *\n她笑了。\n")
        val segments = speechSegments(content, 0)
        assertEquals(3, segments.size)
        assertEquals("第1章 开始", segments[0].text)
        assertEquals("他走进来。", segments[1].text)
        assertEquals("她笑了。", segments[2].text)
        val slice = content.utf8.copyOfRange(segments[2].byteStart, segments[2].byteEnd)
        assertEquals("她笑了。", slice.toString(Charsets.UTF_8))
    }

    @Test
    fun segmentsStartAtAnchorParagraph() {
        val content = chapterFrom(0, "第1章 开始\n第一段。\n第二段很长，锚点在中间。\n第三段。\n")
        val middle = content.utf8.indexOf("锚点".toByteArray())
        val segments = speechSegments(content, middle)
        assertEquals(2, segments.size)
        assertEquals("第二段很长，锚点在中间。", segments[0].text)
    }

    @Test
    fun segmentsStartAtClickedSentence() {
        val content = chapterFrom(0, "他站起来。她说：“别走。”外面下雨了。\n下一段。\n", heading = false)
        fun from(needle: String) = speechSegments(content, content.utf8.indexOf(needle.toByteArray()))

        var segments = from("别走")
        assertEquals("她说：“别走。”外面下雨了。", segments.first().text)
        assertEquals(content.utf8.indexOf("她说".toByteArray()), segments.first().byteStart)

        segments = from("下雨")
        assertEquals("外面下雨了。", segments.first().text)
        assertEquals(2, segments.size)

        segments = from("站起")
        assertEquals("他站起来。她说：“别走。”外面下雨了。", segments.first().text)
    }

    @Test
    fun longParagraphSplitsAtSentenceEnd() {
        val paragraph = "他说：“走吧。”".repeat(60)
        val content = chapterFrom(0, paragraph + "\n", heading = false)
        val segments = speechSegments(content, 0)
        assertTrue(segments.size >= 2)
        val joined = buildString {
            for (segment in segments) {
                assertTrue(segment.text.length <= MAX_SPEECH_SEGMENT_CHARS)
                assertTrue(segment.text.endsWith("。”")) { segment.text }
                append(segment.text)
            }
        }
        assertEquals(paragraph, joined)
        assertEquals(0, segments.first().byteStart)
        assertEquals(paragraph.toByteArray().size, segments.last().byteEnd)
        for (i in 1 until segments.size) {
            assertEquals(segments[i - 1].byteEnd, segments[i].byteStart)
        }
    }

    @Test
    fun gbkSampleImportsAsGb18030() {
        val path = temp.resolve("plain-gbk.txt").toFile()
        val gbk = ArrayList<Byte>()
        val line = "山风穿过竹林。\n".toByteArray(charset("GB18030"))
        while (gbk.size < 9000) {
            line.forEach { gbk.add(it) }
        }
        path.writeBytes(gbk.toByteArray())
        val opened = importTxt(path.absolutePath, temp.toString())
        assertTrue(opened.isSuccess) { opened.errorOrNull()?.message.orEmpty() }
        assertEquals("GB18030", opened.valueOrNull()!!.book.sourceEncoding)
        val text = readChapter(opened.valueOrNull()!!, 0).valueOrNull()!!.utf8.toString(Charsets.UTF_8)
        assertTrue(text.contains("山风穿过竹林。"))
    }

    @Test
    fun utf16LeAndBeBomRoundTrip() {
        val le = temp.resolve("le.txt").toFile()
        le.writeBytes(byteArrayOf(0xFF.toByte(), 0xFE.toByte()) + "你好\n".toByteArray(Charsets.UTF_16LE))
        val leBook = importTxt(le.absolutePath, temp.toString())
        assertTrue(leBook.isSuccess) { leBook.errorOrNull()?.message.orEmpty() }
        assertEquals("UTF-16LE", leBook.valueOrNull()!!.book.sourceEncoding)
        assertTrue(readChapter(leBook.valueOrNull()!!, 0).valueOrNull()!!.utf8.toString(Charsets.UTF_8).contains("你好"))

        val be = temp.resolve("be.txt").toFile()
        be.writeBytes(byteArrayOf(0xFE.toByte(), 0xFF.toByte()) + "你好\n".toByteArray(Charsets.UTF_16BE))
        val beBook = importTxt(be.absolutePath, temp.toString())
        assertTrue(beBook.isSuccess) { beBook.errorOrNull()?.message.orEmpty() }
        assertEquals("UTF-16BE", beBook.valueOrNull()!!.book.sourceEncoding)
        assertTrue(readChapter(beBook.valueOrNull()!!, 0).valueOrNull()!!.utf8.toString(Charsets.UTF_8).contains("你好"))
    }

    @Test
    fun utf8BomFileImportsWithoutBomInBody() {
        val path = temp.resolve("bom.txt").toFile()
        path.writeBytes(byteArrayOf(0xEF.toByte(), 0xBB.toByte(), 0xBF.toByte()) + "第1章 甲\n你好\n".toByteArray())
        val opened = importTxt(path.absolutePath, temp.toString())
        assertTrue(opened.isSuccess) { opened.errorOrNull()?.message.orEmpty() }
        val book = opened.valueOrNull()!!
        assertEquals("UTF-8", book.book.sourceEncoding)
        assertEquals(3L, book.bodyOffset)
        assertEquals("第1章 甲", book.chapters.first().title)
        val text = readChapter(book, 0).valueOrNull()!!.utf8.toString(Charsets.UTF_8)
        assertTrue(text.startsWith("第1章 甲"))
        assertFalse(text.contains("\uFEFF"))
    }

    @Test
    fun fileWithoutHeadingsBecomesOneChapter() {
        val path = temp.resolve("plain.txt").toFile()
        path.writeText("只有正文，没有标题。\n第二段。\n")
        val opened = importTxt(path.absolutePath, temp.toString())
        assertTrue(opened.isSuccess) { opened.errorOrNull()?.message.orEmpty() }
        val chapters = opened.valueOrNull()!!.chapters
        assertEquals(1, chapters.size)
        assertEquals("正文", chapters.first().title)
        assertFalse(chapters.first().synthetic)
    }

    @Test
    fun oneVeryLongLineSplitsParagraphs() {
        val path = temp.resolve("long-line.txt").toFile()
        path.writeText("文".repeat(5000))
        val opened = importTxt(path.absolutePath, temp.toString())
        assertTrue(opened.isSuccess) { opened.errorOrNull()?.message.orEmpty() }
        val content = readChapter(opened.valueOrNull()!!, 0).valueOrNull()!!
        assertTrue(content.paragraphs.size > 1)
        assertTrue(content.paragraphs.all { it.codePoints <= MAX_PARAGRAPH_CODE_POINTS })
        assertEquals("文".repeat(5000), content.paragraphs.joinToString("") { paragraphText(content.utf8, it) })
    }

    @Test
    fun cancelAndProgressAreReported() {
        val path = temp.resolve("story.txt").toFile()
        path.writeText("第1章 甲\n你好\n")
        val seen = ArrayList<Double>()
        val opened = importTxt(path.absolutePath, temp.toString(), progress = ImportProgress { fraction, _ ->
            seen.add(fraction)
        })
        assertTrue(opened.isSuccess)
        assertTrue(seen.isNotEmpty())
        assertEquals(1.0, seen.last())

        val cancelled = importTxt(path.absolutePath, temp.toString(), cancel = AtomicBoolean(true))
        assertTrue(cancelled.errorOrNull()?.cancelled == true)
        assertEquals("已取消", cancelled.errorOrNull()?.message)
    }

    private fun chaptersOf(text: ByteArray, pattern: String = ""): List<ChapterIndex> {
        val chapters = detectChapters(text, AtomicBoolean(false), pattern)
        assertTrue(chapters.isSuccess) { chapters.errorOrNull()?.message.orEmpty() }
        return chapters.valueOrNull()!!
    }

    private fun transcode(data: ByteArray, encoding: DetectedEncoding, chunkTarget: Long = 1): ByteArray {
        val path = File.createTempFile("out", ".utf8", temp.toFile())
        path.delete()
        val written = transcodeToCacheFile(data, encoding, path, chunkTarget, AtomicBoolean(false))
        assertTrue(written.isSuccess) { written.errorOrNull()?.message.orEmpty() }
        return path.readBytes()
    }

    private fun encoding(canonical: String, iconvName: String, cut: NewlineCut) =
        DetectedEncoding(canonical, iconvName, 0, cut)
}

private fun bytes(vararg values: Int): ByteArray = ByteArray(values.size) { values[it].toByte() }

private fun paragraphText(utf8: ByteArray, paragraph: Paragraph): String =
    utf8.copyOfRange(paragraph.byteStart, paragraph.byteEnd).toString(Charsets.UTF_8)

private fun chapterFrom(index: Int, text: String, heading: Boolean = true): ChapterContent {
    val utf8 = text.toByteArray()
    val chapter = ChapterIndex(
        index = index,
        title = if (heading) "第${index + 1}章" else "正文",
        byteEnd = utf8.size.toLong(),
    )
    return makeChapterContent(chapter, utf8)
}

private fun ByteArray.indexOf(needle: ByteArray): Int {
    if (needle.isEmpty()) {
        return 0
    }
    outer@ for (i in 0..size - needle.size) {
        for (j in needle.indices) {
            if (this[i + j] != needle[j]) {
                continue@outer
            }
        }
        return i
    }
    return -1
}

private fun List<Byte>.containsSequence(needle: List<Byte>): Boolean {
    if (needle.isEmpty()) {
        return true
    }
    outer@ for (i in 0..size - needle.size) {
        for (j in needle.indices) {
            if (this[i + j] != needle[j]) {
                continue@outer
            }
        }
        return true
    }
    return false
}
