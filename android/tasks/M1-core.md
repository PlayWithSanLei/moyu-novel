# M1：工程骨架 + `:core` 的 TXT 导入管线

分支：`android/m1-core`（从 main 拉出）。第一笔提交带上 `android/AGENTS.md` 和 `android/tasks/`。

## 要做

1. 建 `android/` Gradle 工程
   - `:core`（Kotlin JVM 库）和 `:app`（Compose 空壳，装到手机上能打开，显示「摸鱼阅读器」占位首页）。
   - applicationId `app.moyureader`，应用名「摸鱼阅读器」。
   - version catalog；Gradle wrapper 提交进仓库。
   - 根目录 `.gitignore` 补上 `.gradle/`、`local.properties`、`*.jks`、`*.keystore`、`.kotlin/`。

2. `:core` 里实现（纯 Kotlin，不依赖 Android）
   - 编码识别 + 转 UTF-8：照 `src/core/Encoding.cpp`，包括 BOM、严格 UTF-8 校验、UTF-16 LE/BE、GB18030/GBK、Big5 等。流式处理，不整本读进内存。
   - 章节识别：照 `src/core/ChapterDetector.cpp`，所有内置规则、自定义正则、标题行 40 字上限、无标题文件的合成章节、超长章拆分。
   - 段落切分：照 `splitParagraphs` 和 `makeChapterContent`，包括标题标记、空行、超长段拆分。
   - TXT 导入接口：输入源文件路径和缓存目录，输出书的元数据、章节索引（UTF-8 字节偏移）和正文缓存文件；另有按章节号读出该章内容的接口。支持取消和进度回调。
   - 朗读分段 `speechSegments`：照 `src/core/SpeechText.cpp`，包括"从点到的那一句的句首开始"。

3. 测试
   - 把 `tests/test_core.cpp`、`tests/test_speech.cpp` 里和上面功能相关的用例移植成 JUnit。
   - 另外补：GBK、UTF-16 LE/BE、带 BOM 的样本；无章节标题的文件；一行超长的文件。
   - 大文件：在测试里现场生成约 20MB 的 TXT（不要把大文件提交进仓库，生成方式可参考 `tools/gen_test_book.py`），断言导入耗时和峰值内存在合理范围内。单独打标签，默认测试集可以跳过它，但交付前要跑一次并在说明里写结果。

## 不做

阅读界面、Room、听书播放、EPUB 等其他格式。这些在后面的任务里做。

## 验收

- 在 `android/` 下 `./gradlew :core:test :app:testDebugUnitTest :app:assembleDebug :app:lintDebug` 全部通过。
- 交付说明里列出：移植了哪些测试用例、哪些没移植及原因、和桌面版行为不一致的地方、大文件测试的耗时和内存数字。
