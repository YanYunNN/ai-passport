# FoloToy AI Passport 一体化烧录说明

最终用户只需要选择一个文件：

```text
FoloToy-AI-Passport_0x0_all-in-one.bin
```

它的烧录偏移固定为 **`0x0`**，适用于 **ESP32-C3 / 8 MB Flash / DIO / 80 MHz**。

## 首次迁移提示

本版本将 NVS（Wi-Fi 凭据、亮度、时间和 Wi-Fi 开关）从旧地址 `0x9000` 移到 `0x187000`，以支持后续单 BIN 升级保留配置。

因此，**第一次刷入本一体化版本后需要重新配网一次**。完成一次配网后，后续刷入同类 all-in-one BIN 不会覆盖新的 NVS，Wi-Fi 与设置会保留。

## 图形化烧录工具

选择：

| 字段 | 值 |
| --- | --- |
| Chip | ESP32-C3 |
| BIN 文件 | `FoloToy-AI-Passport_0x0_all-in-one.bin` |
| Flash 偏移 | `0x0` |
| Flash size | 8 MB |
| Flash mode | DIO |
| Flash frequency | 80 MHz |

不要勾选 **Erase all**、`erase_flash` 或类似选项；它们会清空新位置的 NVS，设备需要重新配网。

## 命令行烧录

在项目根目录执行，将串口替换为实际设备：

```sh
python -m esptool --chip esp32c3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 deploy/FoloToy-AI-Passport_0x0_all-in-one.bin
```

如果无法连接设备：按住 **BOOT**，短按 **RESET**，松开 **BOOT** 后重试。

## 验收 Wi-Fi 持久化

1. 第一次烧录后重新完成 Wi-Fi 配网，等待显示 `ONLINE`。
2. 只按 RESET 或断电重启，不运行烧录工具；设备应自动重连。
3. 用新的 `FoloToy-AI-Passport_0x0_all-in-one.bin` 升级。
4. 确认 Wi-Fi、亮度、时间和 Wi-Fi 开关仍保留。
