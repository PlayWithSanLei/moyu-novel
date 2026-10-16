#!/bin/bash
# 先完整打开一次，等章节索引写完并退出，再量启动到画出正文。
# 用法：tools/cold_start.sh [txt路径]
# 设置 HOME 可以避开 ~/Library 里的真实数据库。

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FILE="${1:-$ROOT/bench/data/gb18030_15mb.txt}"
BIN="${BIN:-$ROOT/build/release/MoyuReader.app/Contents/MacOS/MoyuReader}"
export QT_PLUGIN_PATH="${QT_PLUGIN_PATH:-$HOME/Qt/6.8.3/macos/plugins}"

if [[ ! -x "$BIN" ]]; then
  echo "找不到 $BIN" >&2
  exit 1
fi
if [[ ! -f "$FILE" ]]; then
  echo "找不到 $FILE" >&2
  exit 1
fi

echo "1/2 写入索引"
MOYU_QUIT_AFTER_INDEX=1 "$BIN" "$FILE"
echo "2/2 启动到画出正文"
MOYU_TIMING=1 MOYU_QUIT_AFTER_PAINT=1 "$BIN" "$FILE"
