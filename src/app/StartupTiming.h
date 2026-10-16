#pragma once

namespace moyu {

// 从 main 入口记时，不是静态初始化。MOYU_TIMING=1 时，正文第一次画出来打印毫秒。
// 测冷启动要分两次：先 MOYU_QUIT_AFTER_INDEX=1 等目录写完并正常退出，
// 再 MOYU_TIMING=1 MOYU_QUIT_AFTER_PAINT=1 量启动到画出正文。
// 两个退出开关同时开时，以索引写完为准，避免在预览那一帧退出。
void markStartup();
void noteTextPainted();
void noteIndexReady();

}  // namespace moyu
