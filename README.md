# 摸鱼阅读器

macOS 上的阅读器，支持 TXT、EPUB、无 DRM 的 MOBI / AZW3，以及 FB2、HTML 和 Markdown。界面上的名字是「摸鱼阅读器」，可执行文件名是 `MoyuReader`。带 DRM 的书会提示不能打开，不会尝试解密。

## 快捷键

| 功能 | 默认 |
| --- | --- |
| 老板键，显示或隐藏 | Option + Q |
| 下一页 | Option + → |
| 上一页 | Option + ← |
| 切换鼠标穿透 | Option + E |
| 打开目录 | Command + T |
| 搜索 | Command + F |
| 返回上一位置 | Command + [ |
| 添加书签 | Command + D |
| 跳转 | Command + G |

可以在「设置…」里改老板键、翻页键和穿透键。Cmd+Q、Cmd+Tab 这类系统组合键录不进去。穿透的快捷键如果注册失败，穿透不会打开。菜单栏图标里有「关闭穿透」，Dock 隐藏后退出、设置、置顶也在那里。

外观预设有深色、白色、护眼绿、羊皮纸，也可以自己选背景色和文字色。改完立刻生效。

「文件」菜单可以一次打开多本书，拖进窗口也一样。书架按最后阅读时间排列，显示书名、进度和最后阅读时间。删掉书架上的书只去掉记录和缓存，不删原文件。原文件被移走或删掉时，会让你重新选一次。下次启动会打开上次读的书，并停在上次的位置。

TXT 的章节规则仍然是内置的那几套。设置里可以另外写一条章节正则，点「重新生成目录」后按它重切当前这本 TXT。一行去掉首尾空白后超过 40 个字，不会当成标题。正则写错时，打开书仍用内置规则；重新生成时会提示正则无效。

「阅读」菜单里可以切翻页模式和自动滚动，也可以打开搜索、书签和跳转。翻页从当前锚点逐行填一页，不预先给整章分页。滚动时上一章、当前章和下一章在同一份文档里，滚过边界只在远处补上一章或卸掉一章。自动滚动只在打开时跑定时器；老板键藏起窗口时暂停，再显示时继续。搜索在后台分块进行，可以取消，结果分批出现。点一条结果会跳过去，并把关键字高亮。

## 冷启动

先等目录写进数据库并正常退出，再量从 `main` 到画出正文。不要两个开关一起开。

```bash
export QT_PLUGIN_PATH="$HOME/Qt/6.8.3/macos/plugins"
BIN=build/release/MoyuReader.app/Contents/MacOS/MoyuReader
MOYU_QUIT_AFTER_INDEX=1 "$BIN" bench/data/gb18030_15mb.txt
MOYU_TIMING=1 MOYU_QUIT_AFTER_PAINT=1 "$BIN" bench/data/gb18030_15mb.txt
```

stderr 里看 `moyu_startup_paint_ms`。`moyu_db_open_ms` 后面的 `wal_bytes` 是打开前残留的 WAL。索引提交后会做 checkpoint。UTF-8 缓存是 fsync 之后才换上的：强制结束如果发生在缓存写完之后、章节事务提交之前，下次直接用这份缓存，只重扫章节。缓存文件还没出现时，才会整本再转码。

## 编译

需要 CMake 3.24 以上，以及 Qt 6.8 的 Widgets。不要用 Homebrew 的 Qt 5。

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/macos"
cmake --build build/debug -j 8
```

Release 用 `-DCMAKE_BUILD_TYPE=Release`，产物是 arm64 和 x86_64 的通用二进制。Debug 只编本机架构，并打开 ASan 和 UBSan。

打包成可以拷走的应用：

```bash
./scripts/build.sh
```

这一条会做 Release 编译、用 `macdeployqt` 收齐 Qt 库、`codesign --force --deep -s -` 做 ad-hoc 签名，再打出 `build/release/MoyuReader.dmg`。不需要苹果开发者证书。Qt 不在默认目录时，先设 `QT_PREFIX`。

## 依赖下载失败时

xxHash、uchardet、minizip-ng、pugixml 和 libmobi 第一次配置会从 Git 拉取。网络不好时，把对应版本的源码放到本地，用 `FETCHCONTENT_SOURCE_DIR_<NAME>` 指过去。名字是 `FetchContent_Declare` 里那个名字的大写。

- xxHash 0.8.4：`-DFETCHCONTENT_SOURCE_DIR_XXHASH=/path/to/xxHash`  
  它的 CMake 工程在 `build/cmake`。
- uchardet 0.0.8：`-DFETCHCONTENT_SOURCE_DIR_UCHARDET=/path/to/uchardet`  
  这里只用它的源码，不用它自带的 CMake 工程。
- minizip-ng 4.0.8：`-DFETCHCONTENT_SOURCE_DIR_MINIZIP=/path/to/minizip-ng`
- pugixml 1.14：`-DFETCHCONTENT_SOURCE_DIR_PUGIXML=/path/to/pugixml`
- libmobi 0.12：`-DFETCHCONTENT_SOURCE_DIR_LIBMOBI=/path/to/libmobi`  
  这里只编译读文件要用的源文件，不编它自带的命令行工具，也不调用解密。

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/macos" \
  -DFETCHCONTENT_SOURCE_DIR_XXHASH="$HOME/src/xxHash" \
  -DFETCHCONTENT_SOURCE_DIR_UCHARDET="$HOME/src/uchardet"
```
