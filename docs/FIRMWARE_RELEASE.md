# Firmware Release Guide

目标硬件为 **ESP32-C3 / 8 MB Flash**。发布给普通升级使用的文件统一位于项目根目录的 [`deploy/`](../deploy/)；其文件名包含实际 Flash 偏移，避免烧录时混淆。

设备 NVS 区间是 `0x9000`–`0xefff`，其中保存 Wi-Fi 凭据、亮度、时间和 Wi-Fi 开关。**正常升级不得擦除或写入该区域。**

## 1. 发布要求

- ESP-IDF **v5.5.3**。
- CMake 可通过 `PATH` 使用。
- 已审核的源代码版本。
- 目标硬件上的实际验收；构建通过不代表硬件已验证。

macOS 环境示例：

```sh
export IDF_PATH="$HOME/esp/esp-idf-v5.5.3"
source "$IDF_PATH/export.sh"
```

## 2. 构建与检查

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

确认构建输出显示应用能够装入 factory 分区。

## 3. 整理 deploy 交付文件

每次成功构建后，将三个分段文件移动到 `deploy/` 并按偏移命名：

```sh
mv build/bootloader/bootloader.bin \
  deploy/FoloToy-AI-Passport_0x0_bootloader.bin
mv build/partition_table/partition-table.bin \
  deploy/FoloToy-AI-Passport_0x8000_partition-table.bin
mv build/FoloToy-AI-Passport.bin \
  deploy/FoloToy-AI-Passport_0x10000_app.bin
```

交付目录必须包含：

| 文件 | Flash 偏移 |
| --- | ---: |
| `deploy/FoloToy-AI-Passport_0x0_bootloader.bin` | `0x0` |
| `deploy/FoloToy-AI-Passport_0x8000_partition-table.bin` | `0x8000` |
| `deploy/FoloToy-AI-Passport_0x10000_app.bin` | `0x10000` |
| `deploy/FLASHING.md` | 烧录与验收说明 |

分别记录交付文件的校验和：

```sh
shasum -a 256 deploy/*.bin
```

## 4. 正常升级：保留 NVS、Wi-Fi 和设备设置

使用三个明确段进行烧录，或按 [`deploy/FLASHING.md`](../deploy/FLASHING.md) 在图形工具中添加相同三行：

```sh
python -m esptool --chip esp32c3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 deploy/FoloToy-AI-Passport_0x0_bootloader.bin \
  0x8000 deploy/FoloToy-AI-Passport_0x8000_partition-table.bin \
  0x10000 deploy/FoloToy-AI-Passport_0x10000_app.bin
```

不要启用 `erase_flash`、**Erase all** 或类似功能。它们会清空 NVS，设备需要重新配网。

## 5. 工厂初始化

连续 raw 合并镜像在段间带有填充字节；从 `0x0` 烧录时会覆盖 `0x9000`–`0xefff` 的 NVS。因此，它只可用于工厂初始化或明确要求清空全部用户配置的场景，不能放入普通升级 `deploy/` 包。

如确实需要工厂镜像，生成文件名必须明确标记会清空 NVS：

```sh
python -m esptool --chip esp32c3 merge_bin \
  --output build/FoloToy-AI-Passport-esp32c3-8mb-factory.bin \
  --format raw --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/FoloToy-AI-Passport.bin
```

## 6. 硬件验收

1. 用 `deploy/` 的三段包完成烧录。
2. 配置 Wi-Fi，等待显示 `ONLINE`。
3. 只通过 RESET 或断电重启，确认自动重连。
4. 再使用相同的三段包升级，确认 Wi-Fi、亮度、时间与 Wi-Fi 开关仍保留。

记录实际板卡上的结果；未完成该流程前，发布状态应标注为未完成硬件验证。
