#!/bin/zsh
# 一条命令：Release 通用二进制、macdeployqt、ad-hoc 签名、dmg。
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
qt_prefix="${QT_PREFIX:-$HOME/Qt/6.8.3/macos}"
build_dir="${BUILD_DIR:-$root/build/release}"
app="$build_dir/MoyuReader.app"
dmg="$build_dir/MoyuReader.dmg"

cmake -S "$root" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$qt_prefix"
cmake --build "$build_dir" --target MoyuReader -j "$(sysctl -n hw.ncpu)"

"$qt_prefix/bin/macdeployqt" "$app"
codesign --force --deep -s - "$app"
rm -f "$dmg"
hdiutil create -volname "摸鱼阅读器" -srcfolder "$app" -ov -format UDZO "$dmg"
echo "已生成 $dmg"
