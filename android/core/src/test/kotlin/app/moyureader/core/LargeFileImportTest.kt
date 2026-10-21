package app.moyureader.core

import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Tag
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.io.TempDir
import java.lang.management.ManagementFactory
import java.lang.management.MemoryType
import java.nio.file.Path
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 默认测试任务排除 large。交付前用 :core:largeFileTest 单独跑。
 * 正文不出现「第」，避免把填充行认成章节。生成的文件只留在临时目录。
 */
class LargeFileImportTest {
    @TempDir
    lateinit var temp: Path

    @Test
    @Tag("large")
    fun importTwentyMegabytesStaysBounded() {
        val path = temp.resolve("twenty-mb.txt").toFile()
        val filler = "山风穿过竹林，带来远处的潮声。\n"
        val target = 20L * 1024 * 1024
        var charsSinceHeading = 8000
        var chapter = 1
        path.outputStream().buffered().use { output ->
            var written = 0L
            while (written < target) {
                val piece = if (charsSinceHeading >= 8000) {
                    val heading = "第${chapter}章 试炼\n"
                    chapter++
                    charsSinceHeading = 0
                    heading
                } else {
                    filler
                }
                val encoded = piece.toByteArray()
                output.write(encoded)
                written += encoded.size
                charsSinceHeading += piece.length
            }
        }
        val fileBytes = path.length()
        assertTrue(fileBytes in target..(target + 64 * 1024)) { "文件大小 $fileBytes" }

        val pools = ManagementFactory.getMemoryPoolMXBeans().filter { it.type == MemoryType.HEAP }
        System.gc()
        pools.forEach { pool -> runCatching { pool.resetPeakUsage() } }
        val before = pools.sumOf { it.usage.used }
        val started = System.nanoTime()
        val opened = importTxt(path.absolutePath, temp.toString(), AtomicBoolean(false))
        val elapsedMs = (System.nanoTime() - started) / 1_000_000
        val peak = pools.sumOf { it.peakUsage.used }
        val delta = peak - before
        println("LARGE_FILE bytes=$fileBytes elapsedMs=$elapsedMs peakHeapBytes=$peak deltaHeapBytes=$delta")

        assertTrue(opened.isSuccess) { opened.errorOrNull()?.message.orEmpty() }
        val imported = opened.valueOrNull()!!
        assertTrue(imported.chapters.size > 10)
        assertEqualsTitle(imported.chapters.first().title)
        val content = readChapter(imported, 0)
        assertTrue(content.isSuccess) { content.errorOrNull()?.message.orEmpty() }
        assertTrue(content.valueOrNull()!!.utf8.toString(Charsets.UTF_8).contains("试炼"))
        assertTrue(elapsedMs < 60_000) { "导入用了 $elapsedMs ms" }
        assertTrue(delta < 96L * 1024 * 1024) { "堆增量 $delta" }
    }

    private fun assertEqualsTitle(title: String) {
        assertTrue(title.startsWith("第1章")) { title }
    }
}
