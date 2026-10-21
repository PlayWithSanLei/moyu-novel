cmake_minimum_required(VERSION 3.24)

# 架构必须在 project() 之前决定，否则编译器探测会锁死在错误的架构上。
# Debug 只用本机架构：ASan 运行库按架构分开，通用二进制加 sanitizer 不适合日常调试。
# Release / RelWithDebInfo / MinSizeRel 才编 arm64;x86_64。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

set(_moyu_release_build OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Release"
   OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
   OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(_moyu_release_build ON)
endif()

if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

if(_moyu_release_build)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING
            "Release builds a universal binary" FORCE)
    endif()
else()
    # 同一构建目录里如果残留了 Release 的通用架构，Debug 会链不上本机的 ASan 运行库。
    if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64" AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
        set(CMAKE_OSX_ARCHITECTURES "" CACHE STRING
            "Debug uses the host architecture" FORCE)
    endif()
endif()

project(MoyuReader VERSION 1.0.1 LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT APPLE)
    message(FATAL_ERROR "摸鱼阅读器只支持 macOS 13 及以上。")
endif()

if(_moyu_release_build)
    option(MOYU_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
else()
    option(MOYU_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" ON)
endif()

if(MOYU_SANITIZE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
    message(FATAL_ERROR
        "MOYU_SANITIZE 不能和通用二进制一起用。\n"
        "Debug 请使用本机架构（不要设置 CMAKE_OSX_ARCHITECTURES），Release 再编 arm64;x86_64。")
endif()

# 不把版本号钉死在某个 6.8.x。官方在线安装器里 6.8 系列的最新开源版即可。
find_package(Qt6 6.8 QUIET CONFIG COMPONENTS Widgets Test Sql Network)
if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "找不到 Qt 6.8 或更高版本（需要 Widgets、Test、Sql 和 Network，都在 qtbase 里）。\n"
        "\n"
        "请用 Qt 官方在线安装器安装 6.8 系列的最新开源版，组件勾选 macOS。\n"
        "安装目录应类似：\n"
        "  \$HOME/Qt/<版本>/macos\n"
        "例如 \$HOME/Qt/6.8.3/macos。版本号以安装器里实际的 6.8 最新补丁为准。\n"
        "\n"
        "配置时任选一种方式告诉 CMake：\n"
        "  cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug \\\n"
        "        -DCMAKE_PREFIX_PATH=\"\$HOME/Qt/<版本>/macos\"\n"
        "或：\n"
        "  cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug \\\n"
        "        -DQt6_DIR=\"\$HOME/Qt/<版本>/macos/lib/cmake/Qt6\"\n"
        "\n"
        "不要使用 Homebrew 的 Qt 5。Apple Silicon 上的 Homebrew Qt 6 也不是通用二进制，\n"
        "Release 的 arm64;x86_64 需要官方安装器提供的 macOS 套件。")
endif()

qt_standard_project_setup()

# macOS 26 及更新的 SDK 不再提供 AGL。Qt 6.8 的 FindWrapOpenGL 找不到库时
# 仍会写上 -framework AGL，链接因此失败。AGL 只被旧的 OpenGL 全屏接口用到，
# Widgets 阅读器不会调用它，从导入目标里拿掉即可。
if(TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_moyu_gl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if(_moyu_gl_libs)
        set(_moyu_gl_filtered "")
        foreach(_moyu_gl_lib IN LISTS _moyu_gl_libs)
            if(NOT _moyu_gl_lib MATCHES "AGL")
                list(APPEND _moyu_gl_filtered "${_moyu_gl_lib}")
            endif()
        endforeach()
        set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_moyu_gl_filtered}")
    endif()
endif()

function(moyu_apply_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Werror>
        $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wconversion>
        $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wshadow>
    )
    if(MOYU_SANITIZE)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fsanitize=address,undefined>
            $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fno-omit-frame-pointer>
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined
        )
    endif()
endfunction()

include(FetchContent)

# xxHash v0.8.4 的 CMake 在 build/cmake，不再是 cmake_unofficial。
# 子目录模式会强制静态库。关掉 xxhsum，避免多编一个用不到的命令行工具。
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG v0.8.4
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(xxhash)

# 不用 uchardet 自己的 CMake：根目录 cmake_minimum_required(3.1) 会被 CMake 4 拒绝，
# 而且它会 enable_testing 并把 test 目录加进 ctest。这里只编译库源文件。
# SOURCE_SUBDIR 指向一个不存在的目录，这样 FetchContent 只下载、不 add_subdirectory。
FetchContent_Declare(
    uchardet
    GIT_REPOSITORY https://gitlab.freedesktop.org/uchardet/uchardet.git
    GIT_TAG v0.0.8
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake
)
FetchContent_MakeAvailable(uchardet)
file(GLOB moyu_uchardet_sources CONFIGURE_DEPENDS
    "${uchardet_SOURCE_DIR}/src/*.cpp"
    "${uchardet_SOURCE_DIR}/src/LangModels/*.cpp"
)
add_library(moyu_uchardet STATIC ${moyu_uchardet_sources})
target_include_directories(moyu_uchardet PUBLIC "${uchardet_SOURCE_DIR}/src")
target_compile_options(moyu_uchardet PRIVATE -w)

# EPUB 用 minizip-ng 读 zip，用 pugixml 读 OPF / NCX / XHTML。
# 关掉用不到的压缩和加密，避免再去拉 bzip2、lzma、zstd。
set(MZ_COMPAT OFF CACHE BOOL "" FORCE)
set(MZ_ZLIB ON CACHE BOOL "" FORCE)
set(MZ_ZLIB_FLAVOR "zlib" CACHE STRING "" FORCE)
set(MZ_BZIP2 OFF CACHE BOOL "" FORCE)
set(MZ_LZMA OFF CACHE BOOL "" FORCE)
set(MZ_ZSTD OFF CACHE BOOL "" FORCE)
set(MZ_LIBCOMP OFF CACHE BOOL "" FORCE)
set(MZ_PPMD OFF CACHE BOOL "" FORCE)
set(MZ_FETCH_LIBS OFF CACHE BOOL "" FORCE)
set(MZ_FORCE_FETCH_LIBS OFF CACHE BOOL "" FORCE)
set(MZ_PKCRYPT OFF CACHE BOOL "" FORCE)
set(MZ_WZAES OFF CACHE BOOL "" FORCE)
set(MZ_OPENSSL OFF CACHE BOOL "" FORCE)
set(MZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MZ_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(MZ_BUILD_FUZZ_TESTS OFF CACHE BOOL "" FORCE)
# xxHash 的 CMake 会把 BUILD_SHARED_LIBS 写成 ON。这里改回静态库，避免阅读器带着 pugixml / minizip 的动态库。
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(
    minizip
    GIT_REPOSITORY https://github.com/zlib-ng/minizip-ng.git
    GIT_TAG 4.0.8
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(minizip)

set(PUGIXML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PUGIXML_BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG v1.14
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(pugixml)

# libmobi 自带的 CMake 会编命令行工具，还要求 libxml2。这里只编译读文件要用的源文件。
# 加密源文件留着是为了认出 DRM。打开时只报错，不调用解密。
FetchContent_Declare(
    libmobi
    GIT_REPOSITORY https://github.com/bfabiszewski/libmobi.git
    GIT_TAG v0.12
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake
)
FetchContent_MakeAvailable(libmobi)
find_package(ZLIB REQUIRED)
add_library(moyu_mobi STATIC
    "${libmobi_SOURCE_DIR}/src/buffer.c"
    "${libmobi_SOURCE_DIR}/src/compression.c"
    "${libmobi_SOURCE_DIR}/src/debug.c"
    "${libmobi_SOURCE_DIR}/src/index.c"
    "${libmobi_SOURCE_DIR}/src/memory.c"
    "${libmobi_SOURCE_DIR}/src/meta.c"
    "${libmobi_SOURCE_DIR}/src/parse_rawml.c"
    "${libmobi_SOURCE_DIR}/src/read.c"
    "${libmobi_SOURCE_DIR}/src/structure.c"
    "${libmobi_SOURCE_DIR}/src/util.c"
    "${libmobi_SOURCE_DIR}/src/write.c"
    "${libmobi_SOURCE_DIR}/src/xmlwriter.c"
    "${libmobi_SOURCE_DIR}/src/opf.c"
    "${libmobi_SOURCE_DIR}/src/encryption.c"
    "${libmobi_SOURCE_DIR}/src/sha1.c"
    "${libmobi_SOURCE_DIR}/src/randombytes.c"
)
target_include_directories(moyu_mobi PUBLIC "${libmobi_SOURCE_DIR}/src")
target_compile_definitions(moyu_mobi PRIVATE
    USE_ENCRYPTION
    USE_XMLWRITER
    USE_ZLIB
    HAVE_UNISTD_H
    HAVE_STRDUP
    PACKAGE_VERSION="0.12"
    MOBI_INLINE=inline
)
target_link_libraries(moyu_mobi PUBLIC ZLIB::ZLIB)
target_compile_options(moyu_mobi PRIVATE -w)

qt_add_library(moyu_core STATIC
    src/core/ChapterDetector.cpp
    src/core/ChapterDetector.h
    src/core/Database.cpp
    src/core/Database.h
    src/core/Encoding.cpp
    src/core/Encoding.h
    src/core/Fingerprint.cpp
    src/core/Fingerprint.h
    src/core/MappedFile.cpp
    src/core/MappedFile.h
    src/core/SpeechText.cpp
    src/core/SpeechText.h
    src/core/TextSearch.cpp
    src/core/TextSearch.h
    src/core/Paths.h
    src/core/Types.h
    src/core/Utf8.cpp
    src/core/Utf8.h
    src/parser/BookParser.h
    src/parser/BodyCache.cpp
    src/parser/BodyCache.h
    src/parser/EpubParser.cpp
    src/parser/EpubParser.h
    src/parser/Markup.cpp
    src/parser/Markup.h
    src/parser/MobiParser.cpp
    src/parser/MobiParser.h
    src/parser/OpenBook.cpp
    src/parser/OpenBook.h
    src/parser/PlainParser.cpp
    src/parser/PlainParser.h
    src/parser/TxtParser.cpp
    src/parser/TxtParser.h
    src/parser/ZipArchive.cpp
    src/parser/ZipArchive.h
)
target_include_directories(moyu_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(moyu_core PUBLIC QT_NO_KEYWORDS)
# MZ_COMPAT 关闭时库目标是 minizip-ng。MINIZIP::minizip 始终指向它，也能带上头文件。
target_link_libraries(moyu_core PUBLIC Qt6::Core Qt6::Sql xxhash moyu_uchardet iconv MINIZIP::minizip pugixml moyu_mobi)
moyu_apply_warnings(moyu_core)

# QT_NO_KEYWORDS 避免 Qt 的 slots 宏污染 AppKit 头文件。
qt_add_library(moyu_ui STATIC
    src/app/Application.cpp
    src/app/Application.h
    src/app/BookSession.cpp
    src/app/BookSession.h
    src/app/Hotkeys.cpp
    src/app/Hotkeys.h
    src/app/MimoSpeech.cpp
    src/app/MimoSpeech.h
    src/app/ReadAloud.cpp
    src/app/ReadAloud.h
    src/app/SpeechEngine.h
    src/app/SpeechSettings.cpp
    src/app/SpeechSettings.h
    src/app/StartupTiming.cpp
    src/app/StartupTiming.h
    src/app/StealthPolicy.h
    src/reader/ReaderView.cpp
    src/reader/ReaderView.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/ReadingStyle.cpp
    src/ui/ReadingStyle.h
    src/ui/ReadingTools.cpp
    src/ui/ReadingTools.h
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.h
    src/ui/ShelfDialog.cpp
    src/ui/ShelfDialog.h
    src/ui/StatusMenu.cpp
    src/ui/StatusMenu.h
    src/ui/TocDrawer.cpp
    src/ui/TocDrawer.h
    src/ui/WindowGeometry.h
    src/mac/AudioPlayer.h
    src/mac/AudioPlayer.mm
    src/mac/GlobalHotkeys.h
    src/mac/GlobalHotkeys.mm
    src/mac/MacWindow.h
    src/mac/MacWindow.mm
)
target_include_directories(moyu_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(moyu_ui PUBLIC QT_NO_KEYWORDS)
target_link_libraries(moyu_ui PUBLIC moyu_core Qt6::Widgets Qt6::Network "-framework Cocoa" "-framework Carbon" "-framework AVFoundation")
moyu_apply_warnings(moyu_ui)

# 目标名和可执行文件名保持 ASCII。用户看到的中文名只写在 Info.plist 里。
# 图标由 tools/make_icon.py 生成，iconutil 打成 icns，放进 Contents/Resources。
set(MOYU_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/AppIcon.icns")
set_source_files_properties(${MOYU_APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
qt_add_executable(MoyuReader MACOSX_BUNDLE src/main.cpp ${MOYU_APP_ICON})
target_link_libraries(MoyuReader PRIVATE moyu_ui)
moyu_apply_warnings(MoyuReader)
set_target_properties(MoyuReader PROPERTIES
    OUTPUT_NAME "MoyuReader"
    MACOSX_BUNDLE_BUNDLE_NAME "MoyuReader"
    MACOSX_BUNDLE_GUI_IDENTIFIER "app.moyureader"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist"
)

qt_add_executable(moyu_window_tests tests/test_window.cpp)
target_link_libraries(moyu_window_tests PRIVATE moyu_ui Qt6::Test)
moyu_apply_warnings(moyu_window_tests)

qt_add_executable(moyu_core_tests tests/test_core.cpp)
target_link_libraries(moyu_core_tests PRIVATE moyu_core Qt6::Test ZLIB::ZLIB)
moyu_apply_warnings(moyu_core_tests)

qt_add_executable(moyu_reader_tests tests/test_reader.cpp)
target_link_libraries(moyu_reader_tests PRIVATE moyu_ui Qt6::Test)
moyu_apply_warnings(moyu_reader_tests)

qt_add_executable(moyu_speech_tests tests/test_speech.cpp)
target_link_libraries(moyu_speech_tests PRIVATE moyu_ui Qt6::Test)
moyu_apply_warnings(moyu_speech_tests)

# 不进 ctest。大文件的耗时单独跑。
qt_add_executable(moyu_bench bench/bench_open.cpp)
target_link_libraries(moyu_bench PRIVATE moyu_core)
moyu_apply_warnings(moyu_bench)

enable_testing()
add_test(NAME window COMMAND moyu_window_tests)
add_test(NAME core COMMAND moyu_core_tests)
add_test(NAME reader COMMAND moyu_reader_tests)
add_test(NAME speech COMMAND moyu_speech_tests)

# Qt6_DIR 是 .../lib/cmake/Qt6，插件在 .../macos/plugins。
# 测试进程不在 Qt 的 bin 旁边启动，QSQLITE 需要这条路径。
set(_moyu_test_env "QT_PLUGIN_PATH=${Qt6_DIR}/../../../plugins")
if(MOYU_SANITIZE)
    # LSan 会把 Qt 的进程内单例报成泄漏。越界和未定义行为仍然由 ASan / UBSan 拦住。
    list(APPEND _moyu_test_env
        "ASAN_OPTIONS=halt_on_error=1:detect_leaks=0"
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
    )
endif()
set_tests_properties(window core reader PROPERTIES ENVIRONMENT "${_moyu_test_env}")
