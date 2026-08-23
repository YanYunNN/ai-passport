# Firmware Release Guide

目标硬件为 **ESP32-C3 / 8 MB Flash**。普通用户交付物统一位于 [`deploy/`](../deploy/)：一个从 `0x0` 烧录的 all-in-one BIN。

该项目的自定义分区表将 NVS 放在 `0x187000`–`0x18cfff`，位于 factory App 分区之后。因此，连续 raw 镜像从 `0x0` 写入时不会覆盖 NVS，后续单 BIN 固件升级可保留 Wi-Fi 凭据、亮度、时间和 Wi-Fi 开关。

## 1. 一次性分区迁移

旧版固件的 NVS 位于 `0x9000`。本版本将 NVS 移到 `0x187000`，所以设备**第一次**刷入此版 all-in-one 镜像后，旧 Wi-Fi 与应用设置不会迁移，必须重新配网一次。

完成首次迁移和配网后，后续使用本指南的 all-in-one 镜像升级将保留新的 NVS 数据。不要启用 `erase_flash`、**Erase all** 或类似功能。

## 2. 发布要求

- ESP-IDF **v5.5.3**。
- CMake 可通过 `PATH` 使用。
- 已审核的源代码版本。
- 目标硬件上的实际验收；构建通过不代表硬件已验证。

macOS 环境示例：

```sh
export IDF_PATH="$HOME/esp/esp-idf-v5.5.3"
source "$IDF_PATH/export.sh"
```

## 3. 构建与检查

在仓库根目录运行：

```sh
git status --short
source "$HOME/esp/esp-idf-v5.5.3/export.sh"
idf.py set-target esp32c3

cc -std=c11 -Wall -Wextra -Werror -Imain \
  tests/test_ui_pixel_math.c main/ui_pixel_math.c \
  -o /tmp/test_ui_pixel_math
/tmp/test_ui_pixel_math

idf.py build
git diff --check
```

确认构建输出显示应用能够装入 1500 KiB 的 factory 分区，且生成的分区表与 [`partitions.csv`](../partitions.csv) 一致。

## 4. 生成 deploy 一体化镜像

每次成功构建后，生成唯一面向最终用户的升级文件：

```sh
FIRMWARE="deploy/FoloToy-AI-Passport_0x0_all-in-one.bin"

python -m esptool --chip esp32c3 merge_bin \
  --output "$FIRMWARE" \
  --format raw \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 8MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/FoloToy-AI-Passport.bin

shasum -a 256 "$FIRMWARE"
```

交付目录必须包含：

| 文件 | 用途 |
| --- | --- |
| `deploy/FoloToy-AI-Passport_0x0_all-in-one.bin` | 最终用户从 `0x0` 烧录的唯一镜像 |
| `deploy/FLASHING.md` | 图形工具、命令行和持久化验收说明 |

不要将 build 中单独的 bootloader、partition-table 或 app 文件作为普通用户交付物。

## 5. 最终用户烧录

最终用户只需选择：

| 字段 | 值 |
| --- | --- |
| Chip | ESP32-C3 |
| BIN 文件 | `deploy/FoloToy-AI-Passport_0x0_all-in-one.bin` |
| Flash offset | `0x0` |
| Flash size | 8 MB |
| Flash mode | DIO |
| Flash frequency | 80 MHz |

命令行等价写法见 [`deploy/FLASHING.md`](../deploy/FLASHING.md)。

## 6. 硬件验收

1. 首次迁移烧录后重新完成 Wi-Fi 配网，等待显示 `ONLINE`。
2. RESET 或断电重启，确认自动重连。
3. 再次烧录新生成的 all-in-one BIN，确认 Wi-Fi、亮度、时间和 Wi-Fi 开关仍保留。
4. 确认未使用任何整片擦除选项。

记录实际板卡上的结果；未完成该流程前，发布状态应标注为未完成硬件验证。
