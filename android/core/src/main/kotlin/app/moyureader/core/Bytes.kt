package app.moyureader.core

import java.io.Closeable
import java.nio.ByteBuffer
import java.nio.channels.FileChannel

/** 按字节随机读。章节扫描走这个接口，避免把整本拷进一个数组。 */
internal interface Bytes {
    val size: Long

    fun u8(index: Long): Int

    fun read(start: Long, dest: ByteArray, destOffset: Int, length: Int)
}

internal class ArrayBytes(private val data: ByteArray) : Bytes {
    override val size: Long = data.size.toLong()

    override fun u8(index: Long): Int = data[index.toInt()].toInt() and 0xFF

    override fun read(start: Long, dest: ByteArray, destOffset: Int, length: Int) {
        data.copyInto(dest, destOffset, start.toInt(), start.toInt() + length)
    }
}

/**
 * 窗口只保留当前扫描附近的一块。顺序扫章节时每次 refill 往前跳，
 * 不会把几十 MB 的正文留在堆上。
 */
internal class FileBytes(
    private val channel: FileChannel,
    private val origin: Long,
    override val size: Long,
) : Bytes, Closeable {
    private val buf = ByteArray(64 * 1024)
    private var bufStart = Long.MIN_VALUE
    private var bufLen = 0

    override fun u8(index: Long): Int {
        val absolute = origin + index
        if (absolute < bufStart || absolute >= bufStart + bufLen) {
            fill(absolute)
        }
        return buf[(absolute - bufStart).toInt()].toInt() and 0xFF
    }

    override fun read(start: Long, dest: ByteArray, destOffset: Int, length: Int) {
        val absolute = origin + start
        channel.position(absolute)
        val slice = ByteBuffer.wrap(dest, destOffset, length)
        while (slice.hasRemaining()) {
            if (channel.read(slice) < 0) {
                break
            }
        }
        bufStart = Long.MIN_VALUE
    }

    override fun close() {
        channel.close()
    }

    private fun fill(absolute: Long) {
        channel.position(absolute)
        val buffer = ByteBuffer.wrap(buf)
        var read = 0
        while (buffer.hasRemaining()) {
            val n = channel.read(buffer)
            if (n < 0) {
                break
            }
            read += n
        }
        bufStart = absolute
        bufLen = read
    }
}
