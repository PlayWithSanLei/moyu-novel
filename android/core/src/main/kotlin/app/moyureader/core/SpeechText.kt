package app.moyureader.core

data class SpeechSegment(
    val chapter: Int,
    val byteStart: Int,
    val byteEnd: Int,
    val text: String,
)

private fun sentenceEnd(c: Char): Boolean = when (c) {
    '。', '！', '？', '；', '…', '!', '?', ';' -> true
    else -> false
}

/** 句末标点后面紧跟的引号、括号跟着上一句走。 */
private fun closingMark(c: Char): Boolean = when (c) {
    '”', '’', '」', '』', '）', '》', '"', '\'', ')' -> true
    else -> false
}

private fun speakable(text: String): Boolean {
    for (c in text) {
        if (isLetterOrNumber(c)) {
            return true
        }
    }
    return false
}

private fun cleaned(text: String): String {
    val stripped = text.replace("\r", "").replace(HEADING_MARK.toInt().toChar().toString(), "")
    return qtTrim(stripped)
}

private fun cutAfterSentenceEnd(text: String, index: Int): Int {
    var cut = index + 1
    while (cut < text.length && (closingMark(text[cut]) || sentenceEnd(text[cut]))) {
        cut++
    }
    return cut
}

/** fromByte 落在段中间时，从它所在那一句的句首读起。 */
private fun sentenceStart(decoded: DecodedText, fromByte: Int): Int {
    val size = decoded.text.length
    var target = 0
    while (target < size && decoded.utf16ToByte[target] < fromByte) {
        target++
    }
    var start = 0
    for (i in 0 until target) {
        if (sentenceEnd(decoded.text[i])) {
            val cut = cutAfterSentenceEnd(decoded.text, i)
            if (cut <= target) {
                start = cut
            }
        }
    }
    return start
}

private fun pushSegment(
    out: MutableList<SpeechSegment>,
    chapter: Int,
    decoded: DecodedText,
    begin: Int,
    end: Int,
) {
    if (end <= begin) {
        return
    }
    val text = cleaned(decoded.text.substring(begin, end))
    if (!speakable(text)) {
        return
    }
    out.add(
        SpeechSegment(
            chapter = chapter,
            byteStart = decoded.utf16ToByte[begin],
            byteEnd = decoded.utf16ToByte[end],
            text = text,
        ),
    )
}

/** 从 fromByte 所在的那一句起，把本章切成朗读段。空行和只有符号的行跳过。 */
fun speechSegments(content: ChapterContent, fromByte: Int): List<SpeechSegment> {
    val out = ArrayList<SpeechSegment>()
    val total = content.utf8.size
    for (paragraph in content.paragraphs) {
        if (paragraph.kind == ParagraphKind.Blank || paragraph.byteEnd <= fromByte ||
            paragraph.byteEnd <= paragraph.byteStart || paragraph.byteEnd > total
        ) {
            continue
        }
        val slice = content.utf8.copyOfRange(paragraph.byteStart, paragraph.byteEnd)
        val decoded = decodeParagraph(slice, paragraph.byteStart)
        val size = decoded.text.length
        if (decoded.utf16ToByte.size != size + 1) {
            continue
        }
        var begin = if (paragraph.byteStart < fromByte) sentenceStart(decoded, fromByte) else 0
        var lastCut = -1
        // 切完之后下标仍然每次只加 1，和桌面版的 for 循环一致。
        // 句末若在当前下标之后，多走的那几步会重新记下一次句末。
        var i = begin
        while (i < size) {
            if (sentenceEnd(decoded.text[i])) {
                lastCut = cutAfterSentenceEnd(decoded.text, i)
            }
            if (i + 1 - begin >= MAX_SPEECH_SEGMENT_CHARS) {
                var cut = if (lastCut > begin) lastCut else i + 1
                // 硬切时不要把补充平面字符的两个码元拆开。
                if (cut < size && decoded.text[cut - 1].isHighSurrogate()) {
                    cut++
                }
                pushSegment(out, content.index, decoded, begin, cut)
                begin = cut
                lastCut = -1
            }
            i++
        }
        pushSegment(out, content.index, decoded, begin, size)
    }
    return out
}

/** 对齐 QChar::isLetterOrNumber：字母，以及十进制、字母、其他数字。 */
internal fun isLetterOrNumber(c: Char): Boolean = when (Character.getType(c)) {
    Character.UPPERCASE_LETTER.toInt(),
    Character.LOWERCASE_LETTER.toInt(),
    Character.TITLECASE_LETTER.toInt(),
    Character.MODIFIER_LETTER.toInt(),
    Character.OTHER_LETTER.toInt(),
    Character.DECIMAL_DIGIT_NUMBER.toInt(),
    Character.LETTER_NUMBER.toInt(),
    Character.OTHER_NUMBER.toInt(),
    -> true
    else -> false
}

internal fun qtTrim(text: String): String {
    var start = 0
    var end = text.length
    while (start < end && isQtSpace(text[start])) {
        start++
    }
    while (end > start && isQtSpace(text[end - 1])) {
        end--
    }
    return text.substring(start, end)
}

private fun isQtSpace(c: Char): Boolean {
    if (c.code < 32) {
        return c == '\t' || c == '\n' || c == '\u000B' || c == '\u000C' || c == '\r'
    }
    val type = Character.getType(c)
    return type == Character.SPACE_SEPARATOR.toInt() ||
        type == Character.LINE_SEPARATOR.toInt() ||
        type == Character.PARAGRAPH_SEPARATOR.toInt()
}
