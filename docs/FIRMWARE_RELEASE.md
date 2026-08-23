# Firmware Release Guide

目标硬件为 **ESP32-C3 / 8 MB Flash**。普通用户交付物统一位于 [`deploy/`](../deploy/)：一个从 `0x0` 烧录的 all-in-one BIN。

项目使用固定的单 App 分区布局：factory App 从 `0x10000` 开始，大小为 **8100 KiB**（`0x7e9000`）；NVS 位于 `0x7f9000`–`0x7fefff`，PHY 初始化数据位于 `0x7ff000`–`0x7fffff`。NVS 位于 App 最大预留范围之后，因此固件持续增长时不需要改变分区偏移，正常 all-in-one 升级也不会覆盖 Wi-Fi 凭据、亮度、时间、BLE bond 或应用设置。

## 1. 一次性分区迁移

所有使用旧版 NVS 布局（包括 `0x9000` 或 `0x187000`）的设备，第一次刷入本布局的镜像后都必须重新配网一次；旧数据不会自动迁移。完成迁移和配网后，后续使用本指南生成的镜像会保留新的 NVS 数据。

不要启用 `erase_flash`、**Erase all** 或任何整片擦除选项。发布 all-in-one 镜像时也不得使用 `merge_bin --fill-flash-size` 或任何会填充至 `0x7f9000` 之后的选项。

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

## 3. 开发、构建与检查

日常开发只更新 App 并保留 NVS：

```sh
source "$HOME/esp/esp-idf-v5.5.3/export.sh"
idf.py -p /dev/cu.usbmodemXXXX app-flash monitor
```

只有故意改变分区表、bootloader 或首次迁移时才使用完整 `idf.py flash`。不要手动指定 App 偏移；ESP-IDF 从 [`partitions.csv`](../partitions.csv) 读取固定的 `0x10000` 起始地址。

发布构建在仓库根目录运行：

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

确认构建输出显示应用能够装入 `0x7e9000`（8100 KiB）factory 分区，且生成的分区表与 [`partitions.csv`](../partitions.csv) 一致。

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

该命令只合并实际 bootloader、partition table 和 App 文件；**禁止**加入 `--fill-flash-size`。交付目录必须包含：

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
2. 通过受控注册流程将设备凭据写入加密 NVS，等待 NTP 同步。
3. 在 Kiro Passport 页面确认显示 `Relay ready`；RESET 或断电重启后确认会自动重连。
4. 再次烧录新生成的 all-in-one BIN，确认 Wi-Fi、亮度、时间、设备凭据和应用设置仍保留。
5. 触发一个高风险 Kiro 工具调用；设备应显示请求，OK 批准、DOWN 拒绝，断连或超时必须拒绝。
6. 确认未使用任何整片擦除或填充整个 Flash 的打包选项。

记录实际板卡上的结果；未完成该流程前，发布状态应标注为未完成硬件验证。
