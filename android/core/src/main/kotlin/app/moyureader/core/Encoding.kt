package app.moyureader.core

import org.mozilla.universalchardet.UniversalDetector
import java.io.File
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.charset.Charset
import java.nio.charset.CharsetDecoder
import java.nio.charset.CodingErrorAction
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

private const val SAMPLE_BYTES = 64 * 1024
private val REPLACEMENT = byteArrayOf(0xEF.toByte(), 0xBF.toByte(), 0xBD.toByte())
private val transcodeCalls = AtomicLong(0)

data class DetectedEncoding(
    val canonical: String,
    val iconvName: String,
    val bodyOffset: Long,
    val cut: NewlineCut,
) {
    fun passthroughUtf8(): Boolean = canonical == "UTF-8"
}

fun transcodeCallCount(): Long = transcodeCalls.get()

fun resetTranscodeCallCount() {
    transcodeCalls.set(0)
}

/**
 * 先看 BOM，再把前 64KB 交给 juniversalchardet。认不出就报错，不按错误编码硬解。
 * checkUtf8 为真时，判成 UTF-8 / ASCII 会把整个文件再验一遍。验失败就改用 GB18030。
 */
fun detectEncoding(data: ByteArray, checkUtf8: Boolean = true): CoreResult<DetectedEncoding> {
    if (data.isEmpty()) {
        return CoreResult.fail("文件是空的")
    }
    return detectSample(data, data.size.toLong(), checkUtf8) { start, size ->
        utf8WellFormed(ArrayBytes(data), start, size)
    }
}

fun detectEncoding(file: File, checkUtf8: Boolean = true): CoreResult<DetectedEncoding> {
    if (!file.isFile) {
        return CoreResult.fail("找不到文件")
    }
    val size = file.length()
    if (size <= 0L) {
        return CoreResult.fail("文件是空的")
    }
    FileChannel.open(file.toPath(), StandardOpenOption.READ).use { channel ->
        val sampleLen = minOf(size, SAMPLE_BYTES.toLong()).toInt()
        val sample = ByteArray(sampleLen)
        readFully(channel, 0, sample, sampleLen)
        return detectSample(sample, size, checkUtf8) { start, bodySize ->
            FileBytes(channel, 0, size).use { bytes ->
                utf8WellFormed(bytes, start, bodySize)
            }
        }
    }
}

private fun detectSample(
    sample: ByteArray,
    fileSize: Long,
    checkUtf8: Boolean,
    wellFormed: (Long, Long) -> Boolean,
): CoreResult<DetectedEncoding> {
    if (sample.size >= 3 && u8(sample, 0) == 0xEF && u8(sample, 1) == 0xBB && u8(sample, 2) == 0xBF) {
        return utf8OrFallback(3, fileSize, checkUtf8, wellFormed)
    }
    if (sample.size >= 2 && u8(sample, 0) == 0xFF && u8(sample, 1) == 0xFE) {
        return CoreResult.ok(finish("UTF-16LE", 2))
    }
    if (sample.size >= 2 && u8(sample, 0) == 0xFE && u8(sample, 1) == 0xFF) {
        return CoreResult.ok(finish("UTF-16BE", 2))
    }
    val detector = UniversalDetector()
    detector.handleData(sample, 0, sample.size)
    detector.dataEnd()
    val canonical = canonicalName(detector.detectedCharset ?: "")
    if (canonical.isEmpty()) {
        return CoreResult.fail("无法识别编码")
    }
    if (canonical == "UTF-16") {
        return CoreResult.ok(finish(utf16Endian(sample), 0))
    }
    if (canonical == "UTF-8") {
        return utf8OrFallback(0, fileSize, checkUtf8, wellFormed)
    }
    if (iconvNameFor(canonical).isEmpty()) {
        return CoreResult.fail("无法识别编码：$canonical")
    }
    return CoreResult.ok(finish(canonical, 0))
}

private fun utf8OrFallback(
    offset: Long,
    fileSize: Long,
    checkUtf8: Boolean,
    wellFormed: (Long, Long) -> Boolean,
): CoreResult<DetectedEncoding> {
    if (checkUtf8 && offset <= fileSize && !wellFormed(offset, fileSize - offset)) {
        return CoreResult.ok(finish("GB18030", offset))
    }
    return CoreResult.ok(finish("UTF-8", offset))
}

internal fun canonicalName(raw: String): String {
    val name = raw.trim().uppercase().replace('_', '-')
    return when (name) {
        "UTF-8", "ASCII", "US-ASCII", "ANSI-X3.4-1968" -> "UTF-8"
        "GB18030", "GBK", "GB2312", "GB-2312" -> "GB18030"
        "BIG5", "BIG-5", "BIG5-HKSCS", "CP950" -> "BIG5"
        "UTF-16LE", "UTF-16-LE" -> "UTF-16LE"
        "UTF-16BE", "UTF-16-BE" -> "UTF-16BE"
        "UTF-16" -> "UTF-16"
        else -> ""
    }
}

private fun iconvNameFor(canonical: String): String = when (canonical) {
    "GB18030" -> "GB18030"
    // BIG5-HKSCS 是 Big5 的超集，香港增补和欧元符号也能解开。探测名仍然记 BIG5。
    "BIG5" -> "BIG5-HKSCS"
    "UTF-16LE" -> "UTF-16LE"
    "UTF-16BE" -> "UTF-16BE"
    else -> ""
}

private fun cutFor(canonical: String): NewlineCut = when (canonical) {
    "UTF-16LE" -> NewlineCut.Utf16Le
    "UTF-16BE" -> NewlineCut.Utf16Be
    else -> NewlineCut.Bytes
}

private fun finish(canonical: String, offset: Long): DetectedEncoding =
    DetectedEncoding(canonical, iconvNameFor(canonical), offset, cutFor(canonical))

/** 没有 BOM 时，ASCII 区的 0 在奇数位更像 LE，在偶数位更像 BE。 */
private fun utf16Endian(data: ByteArray): String {
    var evenNulls = 0
    var oddNulls = 0
    val sample = minOf(data.size, SAMPLE_BYTES)
    val aligned = sample - (sample % 2)
    var i = 0
    while (i + 1 < aligned) {
        if (u8(data, i) == 0) {
            evenNulls++
        }
        if (u8(data, i + 1) == 0) {
            oddNulls++
        }
        i += 2
    }
    return if (oddNulls >= evenNulls) "UTF-16LE" else "UTF-16BE"
}

private fun charsetFor(iconvName: String): Charset? = when (iconvName) {
    "GB18030" -> Charset.forName("GB18030")
    "BIG5" -> Charset.forName("Big5")
    // JDK 的 Big5-HKSCS 把 A3 E1（欧元）解成替换符，和 iconv 的 BIG5-HKSCS 不一致。
    // x-MS950-HKSCS 带这个映射，Android 的 ICU 也提供同名字符集。
    "BIG5-HKSCS" -> Charset.forName(
        if (Charset.isSupported("x-MS950-HKSCS")) "x-MS950-HKSCS" else "Big5-HKSCS",
    )
    "UTF-16LE" -> Charset.forName("UTF-16LE")
    "UTF-16BE" -> Charset.forName("UTF-16BE")
    else -> null
}

internal interface ByteReader {
    val size: Long

    fun read(position: Long, dest: ByteArray, length: Int)
}

private class ArrayReader(private val data: ByteArray) : ByteReader {
    override val size: Long = data.size.toLong()

    override fun read(position: Long, dest: ByteArray, length: Int) {
        data.copyInto(dest, 0, position.toInt(), position.toInt() + length)
    }
}

private class ChannelReader(private val channel: FileChannel, private val origin: Long, override val size: Long) : ByteReader {
    override fun read(position: Long, dest: ByteArray, length: Int) {
        readFully(channel, origin + position, dest, length)
    }
}

fun transcodeToCacheFile(
    data: ByteArray,
    encoding: DetectedEncoding,
    destination: File,
    chunkTarget: Long,
    cancel: AtomicBoolean,
): CoreResult<Unit> = transcodeReader(ArrayReader(data), encoding, destination, chunkTarget, cancel, null)

internal fun transcodeFile(
    source: File,
    bodyOffset: Long,
    bodySize: Long,
    encoding: DetectedEncoding,
    destination: File,
    chunkTarget: Long,
    cancel: AtomicBoolean,
    progress: ((Long, Long) -> Unit)?,
): CoreResult<Unit> {
    FileChannel.open(source.toPath(), StandardOpenOption.READ).use { channel ->
        return transcodeReader(ChannelReader(channel, bodyOffset, bodySize), encoding, destination, chunkTarget, cancel, progress)
    }
}

private fun transcodeReader(
    reader: ByteReader,
    encoding: DetectedEncoding,
    destination: File,
    chunkTarget: Long,
    cancel: AtomicBoolean,
    progress: ((Long, Long) -> Unit)?,
): CoreResult<Unit> {
    transcodeCalls.incrementAndGet()
    if (reader.size == 0L) {
        return CoreResult.fail("文件是空的")
    }
    if (encoding.iconvName.isEmpty()) {
        return CoreResult.fail("这个编码不需要转码")
    }
    val charset = charsetFor(encoding.iconvName) ?: return CoreResult.fail("无法打开编码 ${encoding.iconvName}")
    val decoder = charset.newDecoder()
        .onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT)
    return writeConverted(destination, cancel) { writer ->
        val window = ByteArray(MAX_WINDOW)
        var position = 0L
        while (position < reader.size) {
            if (cancel.get()) {
                return@writeConverted CoreResult.cancelled()
            }
            val remaining = reader.size - position
            val toRead = minOf(MAX_WINDOW.toLong(), remaining).toInt()
            reader.read(position, window, toRead)
            val chunk = findChunkEnd(window, 0, toRead, chunkTarget, encoding.cut).toInt()
            if (chunk == 0) {
                return@writeConverted CoreResult.fail("转码分块没有前进")
            }
            val atEof = position + chunk >= reader.size
            var consumed = transcodeChunk(decoder, window, chunk, encoding.cut, atEof, writer)
            if (consumed is CoreResult.Err) {
                return@writeConverted consumed
            }
            var taken = (consumed as CoreResult.Ok).value
            if (taken == 0L) {
                val remain = reader.size - position
                if (chunk < remain) {
                    val extra = minOf(4L, remain - chunk).toInt()
                    val extendedLen = chunk + extra
                    val extended = if (toRead >= extendedLen) {
                        window
                    } else {
                        val copy = ByteArray(extendedLen)
                        window.copyInto(copy, 0, 0, chunk)
                        val rest = ByteArray(extra)
                        reader.read(position + chunk, rest, extra)
                        rest.copyInto(copy, chunk)
                        copy
                    }
                    val retryEof = position + extendedLen >= reader.size
                    consumed = transcodeChunk(decoder, extended, extendedLen, encoding.cut, retryEof, writer)
                    if (consumed is CoreResult.Err) {
                        return@writeConverted consumed
                    }
                    taken = (consumed as CoreResult.Ok).value
                }
                if (taken == 0L) {
                    val marked = writer.write(REPLACEMENT, REPLACEMENT.size)
                    if (marked is CoreResult.Err) {
                        return@writeConverted marked
                    }
                    decoder.reset()
                    position += 1
                    progress?.invoke(position, reader.size)
                    continue
                }
            }
            position += taken
            progress?.invoke(position, reader.size)
        }
        writer.finish()
    }
}

fun normalizeNewlinesToCacheFile(
    data: ByteArray,
    destination: File,
    cancel: AtomicBoolean,
): CoreResult<Unit> = normalizeReader(ArrayReader(data), destination, cancel, null)

internal fun normalizeNewlinesFile(
    source: File,
    bodyOffset: Long,
    bodySize: Long,
    destination: File,
    cancel: AtomicBoolean,
    progress: ((Long, Long) -> Unit)?,
): CoreResult<Unit> {
    FileChannel.open(source.toPath(), StandardOpenOption.READ).use { channel ->
        return normalizeReader(ChannelReader(channel, bodyOffset, bodySize), destination, cancel, progress)
    }
}

private fun normalizeReader(
    reader: ByteReader,
    destination: File,
    cancel: AtomicBoolean,
    progress: ((Long, Long) -> Unit)?,
): CoreResult<Unit> {
    if (reader.size == 0L) {
        return CoreResult.fail("文件是空的")
    }
    return writeConverted(destination, cancel) { writer ->
        val step = MAX_WINDOW
        val buffer = ByteArray(step)
        var position = 0L
        while (position < reader.size) {
            if (cancel.get()) {
                return@writeConverted CoreResult.cancelled()
            }
            val length = minOf(step.toLong(), reader.size - position).toInt()
            reader.read(position, buffer, length)
            val written = writer.write(buffer, length)
            if (written is CoreResult.Err) {
                return@writeConverted written
            }
            position += length
            progress?.invoke(position, reader.size)
        }
        writer.finish()
    }
}

internal fun containsByte(file: File, offset: Long, size: Long, needle: Int): Boolean {
    FileChannel.open(file.toPath(), StandardOpenOption.READ).use { channel ->
        val buffer = ByteArray(64 * 1024)
        var left = size
        var position = offset
        while (left > 0) {
            val length = minOf(buffer.size.toLong(), left).toInt()
            readFully(channel, position, buffer, length)
            for (i in 0 until length) {
                if ((buffer[i].toInt() and 0xFF) == needle) {
                    return true
                }
            }
            position += length
            left -= length
        }
    }
    return false
}

private const val MAX_WINDOW = 1 shl 20

private fun writeConverted(
    destination: File,
    cancel: AtomicBoolean,
    body: (Utf8NewlineWriter) -> CoreResult<Unit>,
): CoreResult<Unit> {
    val parent = destination.parentFile ?: return CoreResult.fail("无法创建缓存目录")
    if (!parent.exists() && !parent.mkdirs()) {
        return CoreResult.fail("无法创建缓存目录")
    }
    val partial = File(destination.parentFile, destination.name + ".partial")
    val channel = try {
        FileChannel.open(
            partial.toPath(),
            StandardOpenOption.WRITE,
            StandardOpenOption.CREATE,
            StandardOpenOption.TRUNCATE_EXISTING,
        )
    } catch (_: Exception) {
        return CoreResult.fail("无法写入缓存")
    }
    val writer = Utf8NewlineWriter(channel)
    try {
        val written = body(writer)
        if (written is CoreResult.Err) {
            channel.close()
            partial.delete()
            return written
        }
        if (cancel.get()) {
            channel.close()
            partial.delete()
            return CoreResult.cancelled()
        }
        if (!forceAndClose(channel)) {
            partial.delete()
            return CoreResult.fail("无法把缓存写到磁盘")
        }
        if (destination.exists() && !destination.delete()) {
            partial.delete()
            return CoreResult.fail("无法保存缓存")
        }
        try {
            Files.move(partial.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE)
        } catch (_: Exception) {
            try {
                Files.move(partial.toPath(), destination.toPath(), StandardCopyOption.REPLACE_EXISTING)
            } catch (_: Exception) {
                partial.delete()
                return CoreResult.fail("无法保存缓存")
            }
        }
        return CoreResult.ok(Unit)
    } catch (error: Exception) {
        runCatching { channel.close() }
        partial.delete()
        return CoreResult.fail(error.message ?: "写入缓存失败")
    }
}

private fun forceAndClose(channel: FileChannel): Boolean {
    return try {
        channel.force(true)
        channel.close()
        true
    } catch (_: Exception) {
        runCatching { channel.close() }
        false
    }
}

/**
 * iconv 碰到非法字节只丢掉 1 个（UTF-16 丢掉一个码元），再重置状态机。
 * JDK 解码器报错时给的长度可能把后面的合法字节一起算进去，这里不跟它走。
 */
private fun transcodeChunk(
    decoder: CharsetDecoder,
    data: ByteArray,
    size: Int,
    cut: NewlineCut,
    atEof: Boolean,
    writer: Utf8NewlineWriter,
): CoreResult<Long> {
    decoder.reset()
    val input = ByteBuffer.wrap(data, 0, size)
    val chars = java.nio.CharBuffer.allocate(maxOf(size + 8, 64))
    while (input.hasRemaining()) {
        chars.clear()
        val result = decoder.decode(input, chars, atEof)
        chars.flip()
        if (chars.hasRemaining()) {
            val written = writer.writeChars(chars)
            if (written is CoreResult.Err) {
                return written
            }
        }
        when {
            result.isUnderflow -> {
                if (!atEof) {
                    break
                }
                chars.clear()
                decoder.flush(chars)
                chars.flip()
                if (chars.hasRemaining()) {
                    val flushed = writer.writeChars(chars)
                    if (flushed is CoreResult.Err) {
                        return flushed
                    }
                }
                if (input.hasRemaining()) {
                    val marked = writer.write(REPLACEMENT, REPLACEMENT.size)
                    if (marked is CoreResult.Err) {
                        return marked
                    }
                    input.position(input.limit())
                }
                break
            }
            result.isOverflow -> continue
            result.isMalformed || result.isUnmappable -> {
                val marked = writer.write(REPLACEMENT, REPLACEMENT.size)
                if (marked is CoreResult.Err) {
                    return marked
                }
                val skip = if (cut != NewlineCut.Bytes && input.remaining() >= 2) 2 else 1
                val actual = minOf(skip, input.remaining())
                if (actual <= 0) {
                    return CoreResult.fail("转码失败")
                }
                input.position(input.position() + actual)
                decoder.reset()
            }
            else -> return CoreResult.fail("转码失败")
        }
    }
    return CoreResult.ok(input.position().toLong())
}

/** 跨块记住末尾的 \r，这样 \r\n 被切在两块中间时也只写出一个 \n。 */
private class Utf8NewlineWriter(private val channel: FileChannel) {
    private var pendingCr = false

    fun writeChars(chars: java.nio.CharBuffer): CoreResult<Unit> {
        if (!chars.hasRemaining()) {
            return CoreResult.ok(Unit)
        }
        val bytes = chars.toString().toByteArray(Charsets.UTF_8)
        return write(bytes, bytes.size)
    }

    fun write(data: ByteArray, size: Int): CoreResult<Unit> {
        if (size < 0) {
            return CoreResult.fail("写入缓存失败")
        }
        if (size == 0) {
            return CoreResult.ok(Unit)
        }
        if (!pendingCr && !containsCr(data, size)) {
            return writeAll(data, size)
        }
        val filtered = ByteArray(size + 1)
        var out = 0
        for (i in 0 until size) {
            val byte = data[i]
            if (pendingCr) {
                pendingCr = false
                filtered[out++] = '\n'.code.toByte()
                if (byte == '\n'.code.toByte()) {
                    continue
                }
            }
            if (byte == '\r'.code.toByte()) {
                pendingCr = true
                continue
            }
            filtered[out++] = byte
        }
        if (out == 0) {
            return CoreResult.ok(Unit)
        }
        return writeAll(filtered, out)
    }

    fun finish(): CoreResult<Unit> {
        if (!pendingCr) {
            return CoreResult.ok(Unit)
        }
        pendingCr = false
        return writeAll(byteArrayOf('\n'.code.toByte()), 1)
    }

    private fun writeAll(data: ByteArray, size: Int): CoreResult<Unit> {
        val buffer = ByteBuffer.wrap(data, 0, size)
        return try {
            while (buffer.hasRemaining()) {
                channel.write(buffer)
            }
            CoreResult.ok(Unit)
        } catch (_: Exception) {
            CoreResult.fail("写入缓存失败")
        }
    }
}

private fun containsCr(data: ByteArray, size: Int): Boolean {
    for (i in 0 until size) {
        if (data[i] == '\r'.code.toByte()) {
            return true
        }
    }
    return false
}

private fun readFully(channel: FileChannel, position: Long, dest: ByteArray, length: Int) {
    channel.position(position)
    val buffer = ByteBuffer.wrap(dest, 0, length)
    while (buffer.hasRemaining()) {
        if (channel.read(buffer) < 0) {
            break
        }
    }
}

private fun u8(data: ByteArray, index: Int): Int = data[index].toInt() and 0xFF
