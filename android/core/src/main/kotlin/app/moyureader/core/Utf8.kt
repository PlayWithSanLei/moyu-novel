package app.moyureader.core

enum class NewlineCut { Bytes, Utf16Le, Utf16Be }

internal data class One(val codePoint: Int, val consumed: Int)

internal fun decodeOne(bytes: Bytes, index: Long, end: Long): One {
    if (index >= end) {
        return One(0xFFFD, 0)
    }
    val lead = bytes.u8(index)
    if (lead <= 0x7F) {
        return One(lead, 1)
    }
    if (lead in 0xC2..0xDF) {
        val a = continuation(bytes, index, end, 1)
        if (a < 0) {
            return One(0xFFFD, 1)
        }
        return One(((lead and 0x1F) shl 6) or a, 2)
    }
    if (lead in 0xE0..0xEF) {
        val a = continuation(bytes, index, end, 1)
        val b = continuation(bytes, index, end, 2)
        if (a < 0 || b < 0) {
            return One(0xFFFD, 1)
        }
        // E0 后面必须是 A0–BF，否则是过长编码。ED 后面 A0–BF 是代理项。
        if ((lead == 0xE0 && a < 0x20) || (lead == 0xED && a >= 0x20)) {
            return One(0xFFFD, 1)
        }
        val cp = ((lead and 0x0F) shl 12) or (a shl 6) or b
        return One(cp, 3)
    }
    if (lead in 0xF0..0xF4) {
        val a = continuation(bytes, index, end, 1)
        val b = continuation(bytes, index, end, 2)
        val c = continuation(bytes, index, end, 3)
        if (a < 0 || b < 0 || c < 0) {
            return One(0xFFFD, 1)
        }
        if ((lead == 0xF0 && a < 0x10) || (lead == 0xF4 && a > 0x0F)) {
            return One(0xFFFD, 1)
        }
        val cp = ((lead and 0x07) shl 18) or (a shl 12) or (b shl 6) or c
        if (cp > 0x10FFFF) {
            return One(0xFFFD, 1)
        }
        return One(cp, 4)
    }
    return One(0xFFFD, 1)
}

private fun continuation(bytes: Bytes, index: Long, end: Long, offset: Int): Int {
    val position = index + offset
    if (position >= end) {
        return -1
    }
    val value = bytes.u8(position)
    if ((value and 0xC0) != 0x80) {
        return -1
    }
    return value and 0x3F
}

/** 非法序列返回 1：调用方把它换成 U+FFFD，并且只吃掉这一个字节。 */
fun utf8SequenceLength(data: ByteArray, index: Int): Int {
    if (index >= data.size) {
        return 0
    }
    return decodeOne(ArrayBytes(data), index.toLong(), data.size.toLong()).consumed
}

/** 严格的 UTF-8。非法字节、过长编码、半个序列都返回 false。U+FFFD 本身（EF BF BD）算合法。 */
fun utf8WellFormed(data: ByteArray): Boolean = utf8WellFormed(ArrayBytes(data), 0, data.size.toLong())

internal fun utf8WellFormed(bytes: Bytes, start: Long, size: Long): Boolean {
    if (size == 0L) {
        return true
    }
    val end = start + size
    var cursor = start
    while (cursor < end) {
        val one = decodeOne(bytes, cursor, end)
        if (one.consumed <= 0) {
            return false
        }
        // 非法序列也记成 U+FFFD，但只吃 1 个字节。真正的 U+FFFD 是 3 字节。
        if (one.codePoint == 0xFFFD && one.consumed == 1) {
            return false
        }
        cursor += one.consumed
    }
    return true
}

/** limit < 0 时数完。否则最多数 limit 个，用来判断「是不是已经超过 N」。 */
internal fun countCodePoints(bytes: Bytes, start: Long, end: Long, limit: Int): Int {
    var count = 0
    var cursor = start
    while (cursor < end && (limit < 0 || count < limit)) {
        val consumed = decodeOne(bytes, cursor, end).consumed
        if (consumed <= 0) {
            break
        }
        cursor += consumed
        count++
    }
    return count
}

internal class DecodedText(val text: String, val utf16ToByte: IntArray)

internal fun decodeParagraph(data: ByteArray, byteStart: Int): DecodedText {
    if (data.isEmpty()) {
        return DecodedText("", intArrayOf(byteStart))
    }
    val bytes = ArrayBytes(data)
    val end = data.size.toLong()
    val builder = StringBuilder(data.size)
    val offsets = ArrayList<Int>(data.size + 1)
    var cursor = 0L
    while (cursor < end) {
        val offset = byteStart + cursor.toInt()
        val one = decodeOne(bytes, cursor, end)
        if (one.consumed <= 0) {
            break
        }
        cursor += one.consumed
        if (one.codePoint <= 0xFFFF) {
            builder.append(one.codePoint.toChar())
            offsets.add(offset)
        } else {
            val value = one.codePoint - 0x10000
            builder.append((0xD800 + (value shr 10)).toChar())
            builder.append((0xDC00 + (value and 0x3FF)).toChar())
            offsets.add(offset)
            offsets.add(offset)
        }
    }
    offsets.add(byteStart + data.size)
    return DecodedText(builder.toString(), offsets.toIntArray())
}

private const val MAX_CHUNK_BYTES = 1 shl 20

/**
 * 分块终点相对起点的偏移。窗口里有换行就停在最后一个换行之后；
 * 没有就在上限以内向后找。找不到也不超过 1MB。
 * UTF-16 只在码元等于换行时切开。「上」(U+4E0A) 的字节里含有 0x0A，不能按字节搜。
 */
fun findChunkEnd(data: ByteArray, target: Long, cut: NewlineCut): Long =
    findChunkEnd(data, 0, data.size, target, cut)

internal fun findChunkEnd(data: ByteArray, offset: Int, length: Int, target: Long, cut: NewlineCut): Long {
    if (length <= 0) {
        return 0
    }
    val available = length.toLong()
    var wanted = if (target == 0L) 1L else target
    if (wanted > MAX_CHUNK_BYTES) {
        wanted = MAX_CHUNK_BYTES.toLong()
    }
    val limit = minOf(available, MAX_CHUNK_BYTES.toLong())
    var window = minOf(wanted, available)
    if (cut != NewlineCut.Bytes) {
        window = alignCut(window, cut)
    }
    if (cut != NewlineCut.Bytes && window < 2) {
        val ahead = firstNewline(data, offset, limit.toInt(), cut)
        if (ahead > 0) {
            return ahead.toLong()
        }
        val capped = alignCut(limit, cut)
        return if (capped > 0) capped else minOf(available, 1)
    }
    val inside = lastNewline(data, offset, window.toInt(), cut)
    if (inside > 0) {
        return inside.toLong()
    }
    if (limit > window) {
        val ahead = firstNewline(data, offset + window.toInt(), (limit - window).toInt(), cut)
        if (ahead > 0) {
            return window + ahead
        }
    }
    val capped = alignCut(limit, cut)
    return if (capped > 0) capped else minOf(available, 1)
}

private fun alignCut(size: Long, cut: NewlineCut): Long {
    if (cut == NewlineCut.Bytes || size < 2) {
        return size
    }
    return size - (size % 2)
}

private fun utf16Break(first: Int, second: Int, cut: NewlineCut): Boolean {
    val cr = if (cut == NewlineCut.Utf16Le) first == 0x0D && second == 0x00 else first == 0x00 && second == 0x0D
    val lf = if (cut == NewlineCut.Utf16Le) first == 0x0A && second == 0x00 else first == 0x00 && second == 0x0A
    return cr || lf
}

private fun lastNewline(data: ByteArray, offset: Int, window: Int, cut: NewlineCut): Int {
    if (window <= 0) {
        return 0
    }
    if (cut == NewlineCut.Bytes) {
        for (i in window downTo 1) {
            val byte = data[offset + i - 1].toInt() and 0xFF
            if (byte != 0x0A && byte != 0x0D) {
                continue
            }
            if (byte == 0x0D && i < window && (data[offset + i].toInt() and 0xFF) == 0x0A) {
                return i + 1
            }
            return i
        }
        return 0
    }
    val aligned = window - (window % 2)
    var i = aligned
    while (i >= 2) {
        val first = data[offset + i - 2].toInt() and 0xFF
        val second = data[offset + i - 1].toInt() and 0xFF
        if (utf16Break(first, second, cut)) {
            return i
        }
        if (i == 2) {
            break
        }
        i -= 2
    }
    return 0
}

private fun firstNewline(data: ByteArray, offset: Int, length: Int, cut: NewlineCut): Int {
    if (length <= 0) {
        return 0
    }
    if (cut == NewlineCut.Bytes) {
        for (i in 0 until length) {
            val byte = data[offset + i].toInt() and 0xFF
            if (byte == 0x0A) {
                return i + 1
            }
            if (byte == 0x0D) {
                if (i + 1 < length && (data[offset + i + 1].toInt() and 0xFF) == 0x0A) {
                    return i + 2
                }
                return i + 1
            }
        }
        return 0
    }
    val aligned = length - (length % 2)
    var i = 0
    while (i + 1 < aligned) {
        val first = data[offset + i].toInt() and 0xFF
        val second = data[offset + i + 1].toInt() and 0xFF
        if (utf16Break(first, second, cut)) {
            return i + 2
        }
        i += 2
    }
    return 0
}
