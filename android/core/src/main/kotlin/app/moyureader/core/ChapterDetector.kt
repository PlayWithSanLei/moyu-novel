package app.moyureader.core

import java.util.concurrent.atomic.AtomicBoolean
import java.util.regex.Pattern
import java.util.regex.PatternSyntaxException

private enum class RuleKind { Chinese, English, Aux }

private data class Hit(val lineStart: Long, val trimBegin: Long, val trimEnd: Long, val level: Int)

private data class LineSpan(val begin: Long, val contentEnd: Long, val next: Long)

private val chinesePattern: Pattern =
    Pattern.compile("^第[零一二三四五六七八九十百千万两0-9０-９]+([章节回卷集部篇])")

private val englishPattern: Pattern =
    Pattern.compile("^chapter\\s+(\\d+|[IVXLC]+)\\b", Pattern.CASE_INSENSITIVE)

private val auxWords = arrayOf("序章", "楔子", "引子", "前言", "后记", "尾声", "番外")

private val auxWordBytes = auxWords.map { it.toByteArray(Charsets.UTF_8) }

fun chapterRangesValid(chapters: List<ChapterIndex>, bodySize: Long): Boolean {
    if (chapters.isEmpty() || bodySize == 0L) {
        return false
    }
    if (chapters.first().byteStart != 0L || chapters.last().byteEnd != bodySize) {
        return false
    }
    for (i in chapters.indices) {
        val chapter = chapters[i]
        if (chapter.index != i || chapter.byteEnd <= chapter.byteStart || chapter.byteEnd > bodySize) {
            return false
        }
        if (i + 1 < chapters.size && chapters[i + 1].byteStart != chapter.byteEnd) {
            return false
        }
    }
    return true
}

fun detectChapters(
    utf8: ByteArray,
    cancel: AtomicBoolean? = null,
    customPattern: String = "",
): CoreResult<List<ChapterIndex>> = detectChapters(ArrayBytes(utf8), cancel, customPattern, null)

internal fun detectChapters(
    utf8: Bytes,
    cancel: AtomicBoolean?,
    customPattern: String,
    progress: ((Long, Long) -> Unit)?,
): CoreResult<List<ChapterIndex>> {
    if (utf8.size == 0L) {
        return CoreResult.fail("文件是空的")
    }
    val custom = if (customPattern.isNotEmpty()) {
        try {
            Pattern.compile(customPattern)
        } catch (_: PatternSyntaxException) {
            return CoreResult.fail("章节正则无效")
        }
    } else {
        null
    }
    var nonEmpty = 0
    val counts = IntArray(3)
    var customCount = 0
    var lines = 0
    var cursor = 0L
    val end = utf8.size
    while (cursor < end) {
        if ((lines and 1023) == 0 && cancel?.get() == true) {
            return CoreResult.cancelled()
        }
        lines++
        val line = takeLine(utf8, cursor, end)
        val trimmed = trimLine(utf8, line.begin, line.contentEnd)
        if (line.next == cursor) {
            break
        }
        cursor = line.next
        if (trimmed.first == trimmed.second) {
            continue
        }
        nonEmpty++
        if (custom != null && matchesCustom(custom, utf8, trimmed.first, trimmed.second)) {
            customCount++
        }
        val classified = classify(utf8, trimmed.first, trimmed.second) ?: continue
        counts[classified.first.ordinal]++
    }
    progress?.invoke(end, end * 2)

    val customWins = custom != null && customCount > 0 && !tooDense(customCount, nonEmpty)
    var winner = -1
    var winnerCount = 0
    for (rule in 0 until 3) {
        if (tooDense(counts[rule], nonEmpty)) {
            continue
        }
        if (counts[rule] > winnerCount) {
            winnerCount = counts[rule]
            winner = rule
        }
    }

    val hits = ArrayList<Hit>()
    if (customWins || winner >= 0) {
        cursor = 0L
        lines = 0
        while (cursor < end) {
            if ((lines and 1023) == 0 && cancel?.get() == true) {
                return CoreResult.cancelled()
            }
            lines++
            val lineStart = cursor
            val line = takeLine(utf8, cursor, end)
            val trimmed = trimLine(utf8, line.begin, line.contentEnd)
            if (line.next == cursor) {
                break
            }
            cursor = line.next
            if (trimmed.first == trimmed.second) {
                continue
            }
            val level: Int
            if (customWins) {
                if (!matchesCustom(custom, utf8, trimmed.first, trimmed.second)) {
                    continue
                }
                level = 2
            } else {
                val classified = classify(utf8, trimmed.first, trimmed.second)
                val auxWithPrimary = classified != null && classified.first == RuleKind.Aux &&
                    (winner == RuleKind.Chinese.ordinal || winner == RuleKind.English.ordinal)
                if (classified == null || (classified.first.ordinal != winner && !auxWithPrimary)) {
                    continue
                }
                level = classified.second
            }
            hits.add(Hit(lineStart, trimmed.first, trimmed.second, level))
        }
    }
    progress?.invoke(end * 2, end * 2)

    val chapters = ArrayList<ChapterIndex>()
    if (hits.isEmpty()) {
        chapters.add(ChapterIndex(title = "正文", level = 2, byteStart = 0, byteEnd = end))
    } else {
        val absorb = hits.first().lineStart > 0 && isOnlyWhitespace(utf8, 0, hits.first().lineStart)
        if (hits.first().lineStart > 0 && !absorb) {
            chapters.add(
                ChapterIndex(title = "开头", level = 2, byteStart = 0, byteEnd = hits.first().lineStart),
            )
        }
        for (i in hits.indices) {
            val hit = hits[i]
            chapters.add(
                ChapterIndex(
                    title = decodeUtf8(utf8, hit.trimBegin, hit.trimEnd),
                    level = hit.level,
                    byteStart = if (i == 0 && absorb) 0 else hit.lineStart,
                    byteEnd = if (i + 1 < hits.size) hits[i + 1].lineStart else end,
                ),
            )
        }
    }

    val split = ArrayList<ChapterIndex>(chapters.size)
    for (chapter in chapters) {
        split.addAll(splitOne(utf8, chapter))
    }
    val indexed = split.mapIndexed { index, chapter -> chapter.copy(index = index) }
    if (!chapterRangesValid(indexed, end)) {
        return CoreResult.fail("章节范围不连续")
    }
    return CoreResult.ok(indexed)
}

fun splitParagraphs(utf8: ByteArray): List<Paragraph> {
    if (utf8.isEmpty()) {
        return listOf(Paragraph())
    }
    val bytes = ArrayBytes(utf8)
    val paragraphs = ArrayList<Paragraph>()
    val end = utf8.size.toLong()
    var cursor = 0L
    while (cursor < end) {
        val line = takeLine(bytes, cursor, end)
        var contentBegin = line.begin
        var kind = ParagraphKind.Body
        if (contentBegin < line.contentEnd && bytes.u8(contentBegin) == 0x1E) {
            kind = ParagraphKind.Heading
            contentBegin++
        }
        if (kind == ParagraphKind.Heading && contentBegin == line.contentEnd) {
            if (line.next == line.begin) {
                break
            }
            cursor = line.next
            continue
        }
        val lineStart = contentBegin
        val contentEnd = line.contentEnd
        var scan = contentBegin
        var pieceStart = lineStart
        var points = 0
        while (scan < line.contentEnd) {
            val consumed = decodeOne(bytes, scan, line.contentEnd).consumed
            if (consumed <= 0) {
                break
            }
            scan += consumed
            points++
            if (points >= MAX_PARAGRAPH_CODE_POINTS) {
                pushParagraph(paragraphs, pieceStart, scan, points, kind)
                pieceStart = scan
                points = 0
                kind = ParagraphKind.Body
            }
        }
        if (points > 0 || pieceStart == lineStart) {
            pushParagraph(paragraphs, pieceStart, contentEnd, points, kind)
        }
        if (line.next == line.begin) {
            break
        }
        cursor = line.next
    }
    if (paragraphs.isEmpty()) {
        paragraphs.add(Paragraph())
    }
    return paragraphs
}

fun makeChapterContent(chapter: ChapterIndex, body: ByteArray): ChapterContent =
    makeChapterContent(chapter, ArrayBytes(body))

internal fun makeChapterContent(chapter: ChapterIndex, body: Bytes): ChapterContent {
    val content = ChapterContent(index = chapter.index, title = chapter.title)
    if (chapter.byteEnd < chapter.byteStart || chapter.byteEnd > body.size) {
        content.paragraphs.addAll(splitParagraphs(ByteArray(0)))
        return content
    }
    val length = (chapter.byteEnd - chapter.byteStart).toInt()
    val slice = ByteArray(length)
    if (length > 0) {
        body.read(chapter.byteStart, slice, 0, length)
    }
    content.utf8 = slice
    content.paragraphs.addAll(splitParagraphs(slice))
    if (!chapter.synthetic && chapter.title != "开头" && chapter.title != "正文" &&
        content.paragraphs.isNotEmpty() && content.paragraphs.first().kind != ParagraphKind.Blank
    ) {
        val first = content.paragraphs.first()
        content.paragraphs[0] = first.copy(kind = ParagraphKind.Heading)
    }
    return content
}

private fun tooDense(matches: Int, nonEmpty: Int): Boolean {
    // 两行里只有一个真标题时密度是 100%，不能因此丢掉。
    // 至少 8 处，并且超过非空行的 30%，才当成「整篇都像标题」。
    return matches >= 8 && matches.toLong() * 10 > nonEmpty.toLong() * 3
}

private fun isOnlyWhitespace(bytes: Bytes, begin: Long, end: Long): Boolean {
    var cursor = begin
    while (cursor < end) {
        val value = bytes.u8(cursor)
        if (value == ' '.code || value == '\t'.code || value == '\n'.code || value == '\r'.code ||
            value == '\u000C'.code || value == 0x0B
        ) {
            cursor++
            continue
        }
        if (value == 0xE3 && cursor + 3 <= end && bytes.u8(cursor + 1) == 0x80 && bytes.u8(cursor + 2) == 0x80) {
            cursor += 3
            continue
        }
        return false
    }
    return true
}

private fun trimLine(bytes: Bytes, line: Long, newline: Long): Pair<Long, Long> {
    var left = line
    var right = newline
    while (left < right && (bytes.u8(left) == ' '.code || bytes.u8(left) == '\t'.code)) {
        left++
    }
    while (right > left && (bytes.u8(right - 1) == ' '.code || bytes.u8(right - 1) == '\t'.code || bytes.u8(right - 1) == '\r'.code)) {
        right--
    }
    while (left + 3 <= right && bytes.u8(left) == 0xE3 && bytes.u8(left + 1) == 0x80 && bytes.u8(left + 2) == 0x80) {
        left += 3
    }
    while (right - 3 >= left && bytes.u8(right - 3) == 0xE3 && bytes.u8(right - 2) == 0x80 && bytes.u8(right - 1) == 0x80) {
        right -= 3
    }
    return left to right
}

private fun startsWith(bytes: Bytes, begin: Long, end: Long, needle: ByteArray): Boolean {
    if (end - begin < needle.size) {
        return false
    }
    for (i in needle.indices) {
        if (bytes.u8(begin + i) != (needle[i].toInt() and 0xFF)) {
            return false
        }
    }
    return true
}

private val diByte = "第".toByteArray(Charsets.UTF_8)

private fun chinesePrefix(bytes: Bytes, begin: Long, end: Long): Boolean = startsWith(bytes, begin, end, diByte)

private fun englishPrefix(bytes: Bytes, begin: Long, end: Long): Boolean {
    if (end <= begin || end - begin < 7) {
        return false
    }
    val first = bytes.u8(begin)
    return first == 'C'.code || first == 'c'.code
}

private fun auxPrefix(bytes: Bytes, begin: Long, end: Long): Boolean {
    for (word in auxWordBytes) {
        if (startsWith(bytes, begin, end, word)) {
            return true
        }
    }
    return false
}

private fun classify(bytes: Bytes, begin: Long, end: Long): Pair<RuleKind, Int>? {
    if (!chinesePrefix(bytes, begin, end) && !englishPrefix(bytes, begin, end) && !auxPrefix(bytes, begin, end)) {
        return null
    }
    val length = end - begin
    // 40 个码点最多 160 字节。更长的行不用跑正则。
    if (length > 160 || countCodePoints(bytes, begin, end, 41) > 40) {
        return null
    }
    val line = decodeUtf8(bytes, begin, end)
    if (chinesePrefix(bytes, begin, end)) {
        val matcher = chinesePattern.matcher(line)
        if (matcher.find()) {
            val level = if (matcher.group(1) == "卷") 1 else 2
            return RuleKind.Chinese to level
        }
    }
    if (englishPrefix(bytes, begin, end)) {
        if (englishPattern.matcher(line).find()) {
            return RuleKind.English to 2
        }
    }
    if (auxPrefix(bytes, begin, end) && matchAux(line)) {
        return RuleKind.Aux to 2
    }
    return null
}

private fun matchAux(line: String): Boolean {
    for (word in auxWords) {
        if (line == word) {
            return true
        }
        if (line.length > word.length && line.startsWith(word)) {
            val separator = line[word.length]
            if (separator == ' ' || separator == ':' || separator == '：' || separator == '、' ||
                separator == '，' || separator == ','
            ) {
                return true
            }
        }
    }
    return false
}

private fun takeLine(bytes: Bytes, cursor: Long, end: Long): LineSpan {
    var scan = cursor
    while (scan < end) {
        val value = bytes.u8(scan)
        if (value == '\n'.code || value == '\r'.code) {
            break
        }
        scan++
    }
    val contentEnd = scan
    if (scan < end && bytes.u8(scan) == '\r'.code) {
        scan++
        if (scan < end && bytes.u8(scan) == '\n'.code) {
            scan++
        }
    } else if (scan < end && bytes.u8(scan) == '\n'.code) {
        scan++
    }
    return LineSpan(cursor, contentEnd, scan)
}

private fun nextPieceEnd(bytes: Bytes, begin: Long, end: Long): Long {
    var cursor = begin
    var codePoints = 0
    while (cursor < end) {
        val paragraph = cursor
        var paragraphPoints = 0
        var scan = cursor
        while (scan < end && bytes.u8(scan) != '\n'.code && bytes.u8(scan) != '\r'.code) {
            val consumed = decodeOne(bytes, scan, end).consumed
            if (consumed <= 0) {
                break
            }
            scan += consumed
            paragraphPoints++
            if (codePoints == 0 && paragraphPoints >= SYNTHETIC_CHAPTER_CODE_POINTS) {
                return scan
            }
        }
        if (scan < end && (bytes.u8(scan) == '\n'.code || bytes.u8(scan) == '\r'.code)) {
            if (bytes.u8(scan) == '\r'.code && scan + 1 < end && bytes.u8(scan + 1) == '\n'.code) {
                scan += 2
            } else {
                scan++
            }
            paragraphPoints++
        }
        if (codePoints > 0 && codePoints + paragraphPoints > SYNTHETIC_CHAPTER_CODE_POINTS) {
            return paragraph
        }
        codePoints += paragraphPoints
        cursor = scan
        if (codePoints >= SYNTHETIC_CHAPTER_CODE_POINTS) {
            return cursor
        }
        if (cursor == paragraph) {
            return end
        }
    }
    return end
}

private fun splitOne(body: Bytes, chapter: ChapterIndex): List<ChapterIndex> {
    if (chapter.byteEnd < chapter.byteStart || chapter.byteEnd > body.size) {
        return listOf(chapter)
    }
    val counted = countCodePoints(body, chapter.byteStart, chapter.byteEnd, MAX_CHAPTER_CODE_POINTS + 1)
    if (counted <= MAX_CHAPTER_CODE_POINTS) {
        return listOf(chapter)
    }
    val pieces = ArrayList<ChapterIndex>()
    var cursor = chapter.byteStart
    var pieceNo = 1
    while (cursor < chapter.byteEnd) {
        var pieceEnd = nextPieceEnd(body, cursor, chapter.byteEnd)
        if (pieceEnd <= cursor) {
            val consumed = decodeOne(body, cursor, chapter.byteEnd).consumed
            pieceEnd = if (consumed > 0) cursor + consumed else chapter.byteEnd
        }
        val title = if (pieceNo > 1) chapter.title + " · " + pieceNo else chapter.title
        val level = if (pieceNo > 1 && chapter.level == 1) 2 else chapter.level
        pieces.add(
            chapter.copy(
                title = title,
                level = level,
                byteStart = cursor,
                byteEnd = pieceEnd,
                synthetic = if (pieceNo == 1) chapter.synthetic else true,
            ),
        )
        cursor = pieceEnd
        pieceNo++
    }
    return pieces
}

private fun titleFits(bytes: Bytes, begin: Long, end: Long): Boolean {
    val length = end - begin
    return length <= 160 && countCodePoints(bytes, begin, end, 41) <= 40
}

/** 必须从去掉空白后的行首开始匹配，避免正文中间的字被当成标题。 */
private fun matchesCustom(pattern: Pattern, bytes: Bytes, begin: Long, end: Long): Boolean {
    if (!titleFits(bytes, begin, end)) {
        return false
    }
    val line = decodeUtf8(bytes, begin, end)
    return pattern.matcher(line).lookingAt()
}

private fun decodeUtf8(bytes: Bytes, begin: Long, end: Long): String {
    val length = (end - begin).toInt()
    if (length <= 0) {
        return ""
    }
    val data = ByteArray(length)
    bytes.read(begin, data, 0, length)
    return String(data, Charsets.UTF_8)
}

private fun pushParagraph(
    paragraphs: MutableList<Paragraph>,
    start: Long,
    end: Long,
    codePoints: Int,
    kind: ParagraphKind,
) {
    val byteStart = start.toInt()
    val byteEnd = end.toInt()
    paragraphs.add(
        Paragraph(
            byteStart = byteStart,
            byteEnd = byteEnd,
            codePoints = codePoints,
            kind = if (byteStart == byteEnd) ParagraphKind.Blank else kind,
        ),
    )
}
