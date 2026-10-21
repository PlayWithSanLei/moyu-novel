package app.moyureader.core

import net.openhft.hashing.LongHashFunction
import java.io.File
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.StandardOpenOption

private const val EDGE_BYTES = 4096

data class Fingerprint(val size: Long, val mtimeMs: Long, val hash: Long) {
    /** 缓存文件名用。不含冒号。 */
    fun token(): String {
        val hex = java.lang.Long.toUnsignedString(hash, 16).padStart(16, '0')
        return "${size}_${mtimeMs}_$hex"
    }
}

fun utf8CacheFile(cacheDir: File, token: String): File = File(cacheDir, "$token.utf8")

/** 只读头尾各 4KB。短于 8KB 的文件整份哈希一次，避免头尾重叠把同一段算两遍。 */
fun fingerprintOf(path: File): CoreResult<Fingerprint> {
    if (!path.isFile) {
        return CoreResult.fail("无法读取文件")
    }
    val size = path.length()
    if (size < 0) {
        return CoreResult.fail("无法读取文件大小")
    }
    val hashed = try {
        FileChannel.open(path.toPath(), StandardOpenOption.READ).use { channel ->
            if (size <= EDGE_BYTES * 2) {
                val all = ByteArray(size.toInt())
                readExact(channel, 0, all)
                all
            } else {
                val head = ByteArray(EDGE_BYTES)
                val tail = ByteArray(EDGE_BYTES)
                readExact(channel, 0, head)
                readExact(channel, size - EDGE_BYTES, tail)
                head + tail
            }
        }
    } catch (_: Exception) {
        return CoreResult.fail("读取文件失败")
    }
    val mtime = try {
        Files.getLastModifiedTime(path.toPath()).toMillis()
    } catch (_: Exception) {
        return CoreResult.fail("无法读取文件")
    }
    val hash = LongHashFunction.xx3().hashBytes(hashed)
    return CoreResult.ok(Fingerprint(size, mtime, hash))
}

private fun readExact(channel: FileChannel, position: Long, dest: ByteArray) {
    channel.position(position)
    val buffer = ByteBuffer.wrap(dest)
    while (buffer.hasRemaining()) {
        if (channel.read(buffer) < 0) {
            throw java.io.EOFException()
        }
    }
}
