#!/bin/bash
# 编译并烧录固件（编译失败就不烧），烧完同步一下时间
set -o pipefail
cd "$(dirname "$0")/.."
FQBN="$(cat tools/fqbn.txt)"
PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)"
[ -n "$PORT" ] || { echo "找不到板子：用 USB 线插到 Mac 上"; exit 1; }
# Apple 芯片的 Mac 没装 Rosetta 时，arduino 自带的 ctags 跑不了，用 Homebrew 装的（brew install universal-ctags）
EXTRA=()
[ -x /opt/homebrew/bin/ctags ] && EXTRA=(--build-property "tools.ctags.path=/opt/homebrew/bin")
arduino-cli compile -b "$FQBN" "${EXTRA[@]}" firmware || { echo "compile failed"; exit 1; }
# 检查四段数据都校验通过（不要用 grep -q：它提前退出会掐断烧录）
n=$(arduino-cli upload -b "$FQBN" -p "$PORT" firmware 2>&1 | grep -c "Hash of data verified")
[ "$n" -ge 4 ] && echo "flashed" || { echo "upload failed"; exit 1; }
sleep 2.5 && python3 tools/sync_time.py
