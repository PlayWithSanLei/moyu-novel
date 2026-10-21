# 摸鱼阅读器 Android 版：开发约定

每次开始任务前先读完这份文件，再读对应的任务文件（android/tasks/ 下）。这里的约定优先于你自己的习惯。

## 目标

普通的安卓小说阅读器，能听书。桌面版的老板键、鼠标穿透、无边框窗口、全局热键都不要。

最终功能：导入本地书（先 TXT，再 EPUB / FB2 / HTML / Markdown，最后 MOBI / AZW3）、书架、阅读（滚动和翻页两种模式）、目录、记住进度、书签、全文搜索、外观设置、听书。

## 技术选型（已定，不要换）

- Kotlin，Jetpack Compose + Material 3，单 Activity，Navigation Compose。
- 模块：
  - `:core`：纯 Kotlin/JVM 库，不依赖 Android SDK。放编码识别、章节识别、段落切分、搜索、朗读分段、朗读队列状态机、MiMo 协议。能在电脑上直接跑单元测试。
  - `:app`：Android 应用。界面、Room、DataStore、播放服务。
- 存储：Room 存书、章节索引、进度、书签；DataStore 存设置。MiMo API Key 用 Android Keystore 里的 AES-GCM 密钥加密后再存。
- 网络：OkHttp + kotlinx.serialization。
- 播放：Media3（ExoPlayer + MediaSessionService），前台服务，通知栏、锁屏、蓝牙耳机都能控制。
- 依赖：统一写在 `gradle/libs.versions.toml`，版本全部写死，不用 `+` 或范围，取当时的最新稳定版。能用 Android/Jetpack/JDK 自带的就不加第三方库；要加新库时在交付说明里写理由。
- minSdk 26；compileSdk 和 targetSdk 用所选 AGP 支持的最新稳定版。
- JDK：用 Android Studio 自带的 JBR（Settings → Build, Execution, Deployment → Build Tools → Gradle → Gradle JDK 选它）。命令行构建时让 `JAVA_HOME` 指向同一个 JBR。

## 目录

`android/` 下是独立的 Gradle 工程（`settings.gradle.kts` 在 `android/`）。不要改 `android/` 以外的文件，唯一例外：根目录 `.gitignore` 可以加安卓相关条目。

## 桌面版就是规格说明

桌面版 C++ 代码已经实现并验证过同样的逻辑。移植时以它的行为为准，不要自己发明规则。先读懂对应文件再写 Kotlin。

| 功能 | 桌面版参考 |
| --- | --- |
| 编码识别与转码 | `src/core/Encoding.cpp`、`src/core/Utf8.cpp`。桌面用 uchardet；Kotlin 用 juniversalchardet（同一套 Mozilla 算法），GBK 按 GB18030 解码 |
| 章节识别 | `src/core/ChapterDetector.cpp`：内置规则、标题行最多 40 个字、自定义正则、无标题文件的合成章节 |
| 长度上限 | `src/core/Types.h`：`kMaxChapterCodePoints`、`kSyntheticChapterCodePoints`、`kMaxParagraphCodePoints`、`kHeadingMark` |
| TXT 打开流程、正文缓存 | `src/parser/TxtParser.cpp`、`src/parser/BodyCache.cpp`、`src/parser/OpenBook.cpp` |
| EPUB / FB2 / HTML / Markdown | `src/parser/EpubParser.cpp`、`src/parser/ZipArchive.cpp`、`src/parser/Markup.cpp`、`src/parser/PlainParser.cpp` |
| MOBI / AZW3 | `src/parser/MobiParser.cpp`（底层是 libmobi，C 库） |
| 搜索 | `src/core/TextSearch.cpp` |
| 书库、进度、书签 | `src/core/Database.cpp`、`src/app/BookSession.cpp` |
| 朗读分段 | `src/core/SpeechText.cpp` |
| 朗读队列、预取、重试 | `src/app/ReadAloud.cpp` |
| MiMo 协议与失败分类 | `src/app/MimoSpeech.cpp` |
| 听书设置 | `src/app/SpeechSettings.cpp` |
| 测试用例 | `tests/test_core.cpp`、`tests/test_speech.cpp`、`tests/test_reader.cpp` |

适用的测试用例要移植成 JUnit。觉得桌面版某处行为是 bug 时，不要悄悄"修正"，在交付说明里列出来。

## 核心数据模型（和桌面版一致）

- 导入：系统文件选择器给的是 `content://` URI。先把源文件复制进 App 私有目录，再识别编码、转成 UTF-8 正文缓存文件、建章节索引（UTF-8 字节偏移），存进 Room。
- 阅读位置 = 章节号 + 章内 UTF-8 字节偏移。换字体、字号、屏幕方向，位置都不漂。
- 只按需加载当前章和相邻章，不把整本书读进内存。几十 MB 的 TXT 也要能打开。导入在后台线程跑，有进度，能取消。

## 听书（小米 MiMo）

- 协议照 `src/app/MimoSpeech.cpp`：`POST https://api.xiaomimimo.com/v1/chat/completions`，模型 `mimo-v2.5-tts`。要读的文字放在 `assistant` 消息里，语气描述放在 `user` 消息里，`audio.format = "wav"`，`audio.voice` 默认 `茉莉`（另有 冰糖、苏打、白桦）。`finish_reason = "content_filter"` 表示被内容审核拦下。
- 行为照 `src/app/ReadAloud.cpp`：
  - 一段一个请求，播当前段时合成后面 2 段。
  - 审核拦截：重试 3 次，还不行就跳过并提示。
  - 断网、429、5xx：按 1、2、4 秒退避重试，次数用完后等当前段播完再暂停，提示原因。
  - 401、403：停止并提示去设置里检查 Key。
- 直连，不配置任何代理。
- 用户在设置里自己填 API Key。Key 和书的正文绝不能出现在日志、崩溃信息、异常消息里。
- 可以从用户点按的那一句开始读。跟读标记默认关闭，可以打开并调颜色。
- 锁屏、切到后台继续读；被来电或别的 App 抢走音频焦点时暂停；拔耳机暂停。
- 正在合成、还没出声时要有可见的状态（比如"合成中…"），不能让用户以为卡住了。

## 代码规范

- 注释用中文，少而精，只写"为什么"，不复述代码。
- 不吞异常。用户能看到的错误用中文说清楚原因和怎么办。
- 协程：文件和网络放在 `Dispatchers.IO`，界面线程不阻塞；长任务都能取消。
- 无障碍：可点控件有 `contentDescription`，触控区域不小于 48dp，跟随系统字体缩放。

## 交付规则

- 每个任务在任务文件指定的分支上做，小步提交，提交信息用中文。做完把这个分支推到 origin 的同名分支，供审查。不要改 main，不要 force push。
- 不提交：`local.properties`、签名文件（`*.jks`、`*.keystore`）、任何 API Key。
- 交付前在 `android/` 下跑通：`./gradlew :core:test :app:testDebugUnitTest :app:assembleDebug :app:lintDebug`。跑不通就不算完成。
- 交付说明要写：做了什么、怎么验证的、没做的或偏离约定的地方和原因、需要在真机上人工确认的点。会有人逐行审查代码。
- 审查的人那边没有安卓构建环境，看不到你的运行结果。交付说明末尾附上那条 gradlew 命令的完整输出（至少包括每个任务的结果、测试数量和失败详情），以及大文件测试这类要求的具体数字。
