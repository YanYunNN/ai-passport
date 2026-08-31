#!/usr/bin/env bash
# ============================================================================
# 重新生成中文字体（加粗版）。用于修复 "中文 weight 太轻 / 不明显" 的问题。
#
# 背景
# ----
# 当前 main/ui_font_noto_sc_14.c / ui_font_noto_sc_20.c 由
#   /tmp/NotoSansSC.ttf（Noto Sans SC Regular）生成。Regular 在 14px + Bpp4
#   下细笔画对密集中文不够醒目。要真正加粗，需用同一字重的 Noto Sans SC
#   Bold 重新生成，并沿用原 .c 文件头部注释里的 --symbols 子集（保证 flash
#   用量不变，也避免缺字）。
#
# 前置条件
# - npm install -g lv_font_conv
# - 一份 Noto Sans SC Bold(TTF)，例如 Google Fonts weight 700：
#     https://fonts.google.com/noto/specimen/Noto+Sans+SC
#
# 用法
#   ./scripts/regen_fonts.sh /path/to/NotoSansSC-Bold.ttf
#
# 之后在仓库根：get_idf553 && idf.py build，并核对 build 产物可正常烧录。
# 注意 ESP32-C3 无 PSRAM，14px 字体约 1.76 MB（仅占 flash），Bold 数据量与
# Regular 同量级；若新 build 给 flash/内存告警请报告并回退。
# ============================================================================
set -euo pipefail

BOLD_TTF="${1:?用法: $0 /path/to/NotoSansSC-Bold.ttf}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

command -v lv_font_conv >/dev/null 2>&1 || {
    echo "错误: 未找到 lv_font_conv，请先: npm install -g lv_font_conv" >&2
    exit 1
}
[ -f "$BOLD_TTF" ] || { echo "错误: 字体不存在: $BOLD_TTF" >&2; exit 1; }

# 从旧文件头部注释取出 --symbols 与 --size 之间的子集字符串（原版符号未加引号，
# 直接夹在两者之间），保证与原版逐字节一致。
extract_symbols() {
    sed -n '4p' "$1" | sed -E 's/.*--symbols ([^ ].*) --size.*/\1/'
}

regen() {
    local size="$1" fallback="$2" out="$3"
    local symbols
    symbols="$(extract_symbols "$out")"
    echo "提取到 ${#symbols} 个字符，以 ${size}px 生成 $out ..."
    lv_font_conv \
        --format lvgl \
        --font "$BOLD_TTF" \
        --symbols "$symbols" \
        --size "$size" \
        --bpp 4 \
        --lv-include lvgl.h \
        --lv-fallback "$fallback" \
        --lv-font-name "$(basename "$out" .c)" \
        -o "$out"
}

regen 14 lv_font_montserrat_14 main/ui_font_noto_sc_14.c
regen 20 lv_font_montserrat_20 main/ui_font_noto_sc_20.c

echo "完成。请在仓库根目录执行: get_idf553 && idf.py build"