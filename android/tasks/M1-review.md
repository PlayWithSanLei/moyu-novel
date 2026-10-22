# M1 审查意见

继续在 `android/m1-core` 分支上改，改完推到 origin 同名分支。行号以审查时的提交 17c9848 为准。整体移植质量不错，章节规则、分段、朗读分段、指纹算法和桌面版逐条对得上。下面这些问题要改。

## 必须改

1. **`FileBytes.read()` 每次都把窗口作废**（Bytes.kt:47-57）
   `read()` 末尾把 `bufStart` 设成 `Long.MIN_VALUE`，下一次 `u8()` 就会重新读 64KB。`decodeUtf8`（ChapterDetector.kt:493）每检查一行候选标题就调一次 `read()`：设了自定义正则时，每一行不超过 160 字节的都要读一次；英文书里以 C/c 开头、至少 7 字节的行也一样。一本 20MB 的书要重复读几十 GB。
   改法：目标范围已经在窗口里时直接从 `buf` 拷贝，`read()` 不要作废窗口。
   测试：20MB 的书配一条自定义正则，再来一本 20MB 的英文书，导入时间都要和中文书在同一个量级。

2. **转码时每块开头都 `decoder.reset()`，在安卓上会丢字**（Encoding.kt:452）
   桌面 JDK 的 GB18030/Big5/UTF-16 解码器会把块尾没解完的半个字留在输入里，所以这样写没问题。安卓上这些解码器底层是 ICU，在 `endOfInput=false` 时会把半个字吞进自己的状态，`reset()` 一调就丢了。1MB 内没有换行时，或者 UTF-16 的代理对正好跨在块边界时，都会触发。
   改法：整个文件只用一个解码器，跨块保留状态。只在跳过非法字节之后才 `reset()`，文件结束时再 `decode(true)` 加 `flush`。
   另外，文件末尾半个多字节字的处理要和桌面版一致（Encoding.cpp:332-337，EINVAL）：只写一个 U+FFFD，丢掉剩下的字节。现在是逐字节报错，GB18030 的 `81 30 81` 会变成 `�0�`。

3. **英文章节正则在安卓上的匹配范围和桌面版不一样**（ChapterDetector.kt:17）
   桌面 Qt 用的 PCRE2 没开 UCP，`\d`、`\s`、`\b` 都只认 ASCII。安卓的 `java.util.regex` 基于 ICU，默认按 Unicode 算。结果是 `Chapter 1中` 桌面能匹配、手机上不行，`Chapter １`（全角数字）正好反过来。电脑上跑 JUnit 测不出来，因为 OpenJDK 和桌面版的行为一致。
   改法：字符类写成明确的 ASCII，例如 `^chapter[ \t\n\x0B\f\r]+([0-9]+|[IVXLC]+)(?![A-Za-z0-9_])`，再加 `CASE_INSENSITIVE`。不要用 `UNICODE_CHARACTER_CLASS`。补上述两种情况的测试。

4. **Big5 字符集找不到时会直接抛异常**（Encoding.kt:164-171）
   `Charset.forName` 不在 try 里，系统没有对应字符集时 `UnsupportedCharsetException` 会直接漏出去，也不会退回普通 Big5。`x-MS950-HKSCS` 在安卓 ICU 上是否存在也没核实过。
   改法：按 `x-MS950-HKSCS`、`Big5-HKSCS`、`Big5` 的顺序用 `isSupported` 依次试，失败就返回 `CoreResult.fail`。`"BIG5"` 那个分支永远走不到，删掉。

5. **IO 异常会绕过 `CoreResult` 直接漏出去**
   `detectEncoding` 里的 `FileChannel.open`（Encoding.kt:56）、`containsByte`（:349）、`transcodeFile`（:215）、TxtImport.kt:111，以及 `FileBytes.fill` 的读文件，都没有接异常。没有权限或读失败时调用方会直接崩溃。
   改法：`importTxt` 整体包起来，失败时返回中文的 `fail("无法读取文件")`，原始异常存在 `ParseError` 的 cause 字段里，不要显示给用户。Encoding.kt:425 把系统的英文 `error.message` 直接展示给了用户，也改成中文。

6. **大文件内存测试没有意义，要重写**（LargeFileImportTest.kt:48-67，core/build.gradle.kts:44）
   现在的做法是把各个堆区不同时刻的峰值加在一起，其中 eden 的峰值只反映回收前分配了多少，不代表内存被一直占着。`maxHeapSize = "256m"` 也太宽，就算把整本书读进内存同样能过。
   改法：
   - 单独建一个 Test 任务，`maxHeapSize = "48m"`，加 `jvmArgs("-XX:+UseSerialGC")`。
   - 现场生成 3 个约 20MB 的文件：UTF-8 换行为 LF、UTF-8 换行为 CRLF、GB18030 换行为 CRLF。前两种的区别是会不会走"统一换行写缓存"这条路。
   - 每个文件导入后保留结果，调两次 `System.gc()`，断言常驻堆比导入前多出的部分小于 4MB。
   - 用 `ThreadMXBean.getThreadAllocatedBytes` 打出总分配量，写进交付说明。
   - 第 1 条要求的"自定义正则"和"英文书"两个用例也放在这个任务里。

7. **临时对象太多**
   现在每个字都会 new 一个 `One`（Utf8.kt:5-52），每一行都会 new 一个 `LineSpan`（ChapterDetector.kt:401）和一个装着两个装箱 Long 的 `Pair<Long,Long>`（:287）。20MB 的书会产生几百万个短命对象，手机上会频繁 GC、卡顿。转码时每 1MB 还要新建一个 CharBuffer，再走一遍 `toString().toByteArray()`，大约产生 10MB 垃圾。
   改法：
   - 码点和字节长度打包成一个 Int 或 Long 返回，按行扫描用可变游标代替 `LineSpan` 和 `Pair`。
   - 加一个不分配对象的 `seqLength(bytes, i, end): Int`，给 `splitParagraphs`（ChapterDetector.kt:210）和 `nextPieceEnd`（411-412）用。
   - 转码改成复用一个 CharsetEncoder 和一组固定的缓冲区。
   - 改完以后，第 6 条测出的总分配量应当明显下降。

8. **要有一次在安卓真机或模拟器上的验证**
   第 2、3、4 条的问题只有在 ART 上才会出现。在 `:app` 下加 instrumented test，用 `connectedDebugAndroidTest` 在模拟器上跑通，至少覆盖：
   - GB18030 的 4 字节字正好跨在 1MB 边界上、中间没有换行；
   - UTF-16 代理对跨在块边界上；
   - Big5-HKSCS 的 A3 E1 解成欧元符号 €；
   - 第 3 条的两种英文标题；
   - 能算出 XXH3 指纹。`zero-allocation-hashing` 用了 `sun.misc.Unsafe`，在 ART 上能不能跑没有验证过。

## 应该改

9. **进度和取消**
   常见的 UTF-8 文件，进度只有 0、0.5、0.75、1.0 几档，因为 `detectChapters` 每扫完一遍才回调一次，而且 TxtImport.kt:114 把它传上来的数值丢掉了。改成每处理约 1024 行按位置报一次。整本 UTF-8 校验和 `containsByte` 这两遍全文件扫描也要能检查取消标记。

10. **缓存文件的写法**
    - 临时文件名改成唯一的，现在固定叫 `.partial`，同一本书同时导入两次会撞。
    - 不要先删目标文件再移动（Encoding.kt:407），改成 `Files.move` 加 `ATOMIC_MOVE` 和 `REPLACE_EXISTING`。
    - 缓存可以复用时，跳过那一遍查找 `\r` 的全文件扫描。
    - `cache.name == "$token.utf8"` 永远成立（TxtImport.kt:73），删掉。

11. **读到文件末尾**：`FileBytes.fill` 读到 EOF 后 `u8` 会返回旧数据（Bytes.kt:63-76），`readFully` 也会把旧字节留在窗口里（Encoding.kt:583-591）。导入过程中文件被截断，应该报错，而不是解出一堆乱码。

12. **`FileBytes.close()` 关了不属于它的 channel**（Bytes.kt:59-61）：channel 是调用方传进来的，应该由调用方关。

13. **自定义正则卡死**：Java 正则没有回溯次数上限，写得差的正则可能抛 `StackOverflowError`。捕获后当作这一行没匹配上，在注释里写清这里用的是 Java 正则语法。

14. **空白字符**：SpeechText.kt:154-162 的去空白要加上 U+0085。Qt 的 `isSpace` 把它算作空白，Java 不算。

15. **吞掉的异常**：Fingerprint.kt:45、50 和 TxtImport.kt:179 的 `catch (_: Exception)` 把原始异常丢了，要保留下来作为 cause。

## 补测试

- 转码进行到一半时取消，断言缓存目录里没有留下缓存文件或临时文件。
- 文件内容改了以后，指纹要跟着变。
- CRLF 换行的 UTF-8 文件：要写出缓存，偏移按缓存计算，结果正确。
- XXH3 已知答案测试：`XXH3_64("") == 0x2D06800538D394C2`，再挑一个非空样本和桌面版 `Fingerprint.cpp` 算出的结果比对。
- 一段很短的 GBK 样本的编码识别。juniversalchardet 在短样本上比 uchardet 更容易返回 null，记录实际表现。

## 构建和杂项

- 删掉 app/build.gradle.kts:13 的 `buildToolsVersion = "36.0.0"`，它比 compileSdk 37 低。
- JUnit platform 在 :37 和 :60 配置了两次，删掉一处。
- `allowBackup="true"` 改成 `false`，或者用备份规则排除设置和数据库。后面 API Key 要用 Keystore 加密，从备份恢复的密文解不开。
- 交付说明写清楚：`zero-allocation-hashing:2026.0` 在 Maven Central 上确实存在，以及为什么要加这个库（JDK 自带的库里没有 XXH3）。
- 交付说明写明：纯 UTF-8、没有 `\r` 的文件不生成缓存，直接把源文件当正文用。这和桌面版一致。

## 交付

- 跑通 `./gradlew :core:test :core:largeFileTest :app:testDebugUnitTest :app:assembleDebug :app:lintDebug :app:connectedDebugAndroidTest`，把完整输出贴进交付说明。
- 写出每个大文件用例的耗时、常驻增量和总分配量。
- 列出这份清单里每一条是怎么处理的；没改的写明理由。
