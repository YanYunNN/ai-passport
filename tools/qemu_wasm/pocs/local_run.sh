#!/usr/bin/env bash
# M0: 用 ESP-IDF 官方 QEMU 在本地跑通 FoloToy 固件真实 .bin, 出启动日志.
# 前提: 先 source 好 ESP-IDF(含 qemu-riscv32 / esptool), 且已 idf.py build 产出 bootloader/partition/app。
# 用法: bash tools/qemu_wasm/pocs/local_run.sh [镜像]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="$ROOT/build"
QEMU_C3="qemu-system-riscv32"

if ! command -v "$QEMU_C3" >/dev/null 2>&1; then
    echo "未找到 $QEMU_C3。请先: source \$HOME/esp/esp-idf-v5.5.3/export.sh"
    echo "（必要时 idf.py qemu 会自动拉取官方 qemu-riscv32）"
    exit 2
fi

shift_args="${1:-}"

# 0) 校验关键镜像是否存在
for a in "$BUILD/bootloader/bootloader.bin" \
         "$BUILD/partition_table/partition-table.bin" \
         "$BUILD/FoloToy-AI-Passport.bin"; do
    [ -f "$a" ] || { echo "缺少 $a, 请先 idf.py build"; exit 3; }
done

# 1) 拼成 0x0 完整镜像（rom 从 Flash 0x0 启动, 依赖 bootloader+partition+app 拼接）
MERGED="$(mktemp --suffix=.bin)"
python3 -m esptool merge_bin \
    -o "$MERGED" \
    --fill-flash-size 4MB \
    0x0 "$BUILD/bootloader/bootloader.bin" \
    0x8000 "$BUILD/partition_table/partition-table.bin" \
    0x10000 "$BUILD/FoloToy-AI-Passport.bin"
echo "已生成合并镜像: $MERGED"

# 2) 官方 QEMU esp32c3 启动(串口 stdout, 直接出 console 日志)
echo "=== 启动 QEMU esp32c3 ==="
exec "$QEMU_C3" \
    -machine esp32c3 \
    -nographic \
    -drive file="$MERGED",if=mtd,format=raw \
    -serial stdio