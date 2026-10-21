package app.moyureader.core

enum class BookFormat { Txt, Epub, Mobi, Fb2, Html, Markdown }

data class ParseError(val message: String, val cancelled: Boolean = false)

sealed class CoreResult<out T> {
    data class Ok<T>(val value: T) : CoreResult<T>()
    data class Err(val error: ParseError) : CoreResult<Nothing>()

    val isSuccess: Boolean get() = this is Ok

    fun valueOrNull(): T? = (this as? Ok)?.value

    fun errorOrNull(): ParseError? = (this as? Err)?.error

    companion object {
        fun <T> ok(value: T): CoreResult<T> = Ok(value)

        fun <T> fail(message: String, cancelled: Boolean = false): CoreResult<T> =
            Err(ParseError(message, cancelled))

        fun <T> cancelled(): CoreResult<T> = fail("已取消", cancelled = true)
    }
}

data class Book(
    val filePath: String,
    val fingerprint: String,
    val title: String,
    val author: String = "",
    val format: BookFormat = BookFormat.Txt,
    val sourceEncoding: String,
    val totalBytes: Long,
    val lastReadAt: Long = 0,
)

data class ChapterIndex(
    val index: Int = 0,
    val title: String,
    val level: Int = 2,
    val byteStart: Long = 0,
    val byteEnd: Long = 0,
    val synthetic: Boolean = false,
)

enum class ParagraphKind { Body, Heading, Blank }

/**
 * 行首这个字节表示标题。splitParagraphs 会把它拿掉，正文里不显示。
 * 用一个不会出现在正常 UTF-8 正文里的控制字节，避免再做一张标题偏移表。
 */
const val HEADING_MARK: Byte = 0x1E

data class Paragraph(
    val byteStart: Int = 0,
    val byteEnd: Int = 0,
    val codePoints: Int = 0,
    val kind: ParagraphKind = ParagraphKind.Body,
)

class ChapterContent(
    val index: Int = 0,
    val title: String = "",
    var utf8: ByteArray = ByteArray(0),
    val paragraphs: MutableList<Paragraph> = mutableListOf(),
)

/** 单章超过这个码点数就按约 1 万字拆开。没有标题的 TXT 整本会落成一章。 */
const val MAX_CHAPTER_CODE_POINTS = 100_000
const val SYNTHETIC_CHAPTER_CODE_POINTS = 10_000

/** 再长的段会在界面线程上排版。先按这个长度切开。 */
const val MAX_PARAGRAPH_CODE_POINTS = 2_000

/** 一次合成请求的字数上限。超过就在句末标点处切开。 */
const val MAX_SPEECH_SEGMENT_CHARS = 300
