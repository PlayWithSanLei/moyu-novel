package app.moyureader.core

import java.io.File
import java.nio.channels.FileChannel
import java.nio.file.StandardOpenOption
import java.util.concurrent.atomic.AtomicBoolean

private const val TRANSCODE_CHUNK = 1L shl 20

fun interface ImportProgress {
    fun onProgress(fraction: Double, stage: String)
}

data class ImportedTxt(
    val book: Book,
    val chapters: List<ChapterIndex>,
    val bodyPath: String,
    val bodyOffset: Long,
)

/**
 * 把 TXT 转成 UTF-8 正文缓存（纯 UTF-8 且没有 `\r` 时直接用源文件），并建章节索引。
 * 章节偏移是正文里的 UTF-8 字节偏移。取消和进度在转码、扫描时回调。
 */
fun importTxt(
    sourcePath: String,
    cacheDir: String,
    cancel: AtomicBoolean = AtomicBoolean(false),
    progress: ImportProgress? = null,
    chapterPattern: String = "",
): CoreResult<ImportedTxt> {
    if (!sourcePath.endsWith(".txt", ignoreCase = true)) {
        return CoreResult.fail("目前只支持 TXT")
    }
    val source = File(sourcePath)
    if (!source.isFile) {
        return CoreResult.fail("找不到文件")
    }
    if (source.length() == 0L) {
        return CoreResult.fail("文件是空的")
    }
    if (cancel.get()) {
        return CoreResult.cancelled()
    }
    progress?.onProgress(0.0, "识别编码")
    val fingerprint = fingerprintOf(source)
    if (fingerprint is CoreResult.Err) {
        return fingerprint
    }
    val token = (fingerprint as CoreResult.Ok).value.token()
    val detected = detectEncoding(source)
    if (detected is CoreResult.Err) {
        return detected
    }
    val encoding = (detected as CoreResult.Ok).value
    if (encoding.bodyOffset > source.length()) {
        return CoreResult.fail("文件头损坏")
    }
    val sourceBody = source.length() - encoding.bodyOffset
    if (sourceBody == 0L) {
        return CoreResult.fail("文件是空的")
    }
    if (cancel.get()) {
        return CoreResult.cancelled()
    }

    var bodyFile = source
    var bodyOffset = encoding.bodyOffset
    val needsNewline = encoding.passthroughUtf8() &&
        containsByte(source, encoding.bodyOffset, sourceBody, '\r'.code)
    if (!encoding.passthroughUtf8() || needsNewline) {
        val cache = utf8CacheFile(File(cacheDir), token)
        val reusable = cache.isFile && cache.length() > 0L && cache.name == "$token.utf8"
        if (!reusable) {
            progress?.onProgress(0.02, "转成 UTF-8")
            val written = if (!encoding.passthroughUtf8()) {
                transcodeFile(
                    source,
                    encoding.bodyOffset,
                    sourceBody,
                    encoding,
                    cache,
                    TRANSCODE_CHUNK,
                    cancel,
                ) { done, total ->
                    val fraction = if (total == 0L) 0.5 else 0.02 + 0.48 * done.toDouble() / total.toDouble()
                    progress?.onProgress(fraction.coerceIn(0.0, 0.5), "转成 UTF-8")
                }
            } else {
                normalizeNewlinesFile(source, encoding.bodyOffset, sourceBody, cache, cancel) { done, total ->
                    val fraction = if (total == 0L) 0.5 else 0.02 + 0.48 * done.toDouble() / total.toDouble()
                    progress?.onProgress(fraction.coerceIn(0.0, 0.5), "转成 UTF-8")
                }
            }
            if (written is CoreResult.Err) {
                return written
            }
        }
        bodyFile = cache
        bodyOffset = 0
    }

    if (cancel.get()) {
        return CoreResult.cancelled()
    }
    val bodySize = bodyFile.length() - bodyOffset
    if (bodySize <= 0L) {
        return CoreResult.fail("文件是空的")
    }
    progress?.onProgress(0.5, "识别章节")
    FileChannel.open(bodyFile.toPath(), StandardOpenOption.READ).use { channel ->
        FileBytes(channel, bodyOffset, bodySize).use { bytes ->
            // FileBytes.close 会关掉 channel，外层 use 再关一次没有关系。
            val chapters = detectChapters(bytes, cancel, chapterPattern) { _, _ ->
                progress?.onProgress(0.75, "识别章节")
            }
            val resolved = if (chapters is CoreResult.Err && chapterPattern.isNotEmpty() &&
                chapters.error.message == "章节正则无效"
            ) {
                // 设置里的正则写坏了也不能让书打不开。打开时退回内置规则。
                detectChapters(bytes, cancel, "") { _, _ ->
                    progress?.onProgress(0.9, "识别章节")
                }
            } else {
                chapters
            }
            if (resolved is CoreResult.Err) {
                return resolved
            }
            val list = (resolved as CoreResult.Ok).value
            if (!chapterRangesValid(list, bodySize)) {
                return CoreResult.fail("章节范围不连续")
            }
            val book = Book(
                filePath = source.absolutePath,
                fingerprint = token,
                title = completeBaseName(source.name),
                format = BookFormat.Txt,
                sourceEncoding = encoding.canonical,
                totalBytes = bodySize,
                lastReadAt = System.currentTimeMillis(),
            )
            progress?.onProgress(1.0, "识别章节")
            return CoreResult.ok(
                ImportedTxt(
                    book = book,
                    chapters = list,
                    bodyPath = bodyFile.absolutePath,
                    bodyOffset = bodyOffset,
                ),
            )
        }
    }
}

fun readChapter(imported: ImportedTxt, index: Int): CoreResult<ChapterContent> {
    if (index < 0 || index >= imported.chapters.size) {
        return CoreResult.fail("章节不存在")
    }
    val chapter = imported.chapters[index]
    val body = File(imported.bodyPath)
    if (!body.isFile) {
        return CoreResult.fail("找不到文件")
    }
    return try {
        FileChannel.open(body.toPath(), StandardOpenOption.READ).use { channel ->
            val fileSize = channel.size()
            if (imported.bodyOffset > fileSize) {
                return CoreResult.fail("缓存文件损坏")
            }
            val bodySize = fileSize - imported.bodyOffset
            if (chapter.byteStart > chapter.byteEnd || chapter.byteEnd > bodySize) {
                return CoreResult.fail("章节偏移超出文件")
            }
            FileBytes(channel, imported.bodyOffset, bodySize).use { bytes ->
                CoreResult.ok(makeChapterContent(chapter, bytes))
            }
        }
    } catch (_: Exception) {
        CoreResult.fail("无法读取文件")
    }
}

private fun completeBaseName(name: String): String {
    val dot = name.lastIndexOf('.')
    return if (dot > 0) name.substring(0, dot) else name
}
