# FoloToy AI Passport 一体化烧录说明

最终用户只需要选择一个文件：

```text
FoloToy-AI-Passport_0x0_all-in-one.bin
```

它的烧录偏移固定为 **`0x0`**，适用于 **ESP32-C3 / 8 MB Flash / DIO / 80 MHz**。

## 固定分区与首次迁移

本固件为持续开发预留了固定的 **8100 KiB** App 分区（`0x10000`–`0x7f8fff`），并将 NVS（Wi-Fi 凭据、亮度、时间、BLE bond 和应用设置）固定放在 Flash 末尾的 `0x7f9000`–`0x7fefff`。

这意味着今后的固件增长不需要更改烧录偏移或分区表。由于 NVS 位置与所有旧版不同，**第一次**刷入该布局时必须重新配网一次。完成一次配网后，后续刷入按本项目发布流程生成的同类 all-in-one BIN 会保留 NVS 数据。

不要勾选 **Erase all**、`erase_flash` 或类似选项。发布镜像也不能使用填满整个 Flash 的选项；否则会清空末尾 NVS。

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

## 命令行烧录

在项目根目录执行，将串口替换为实际设备：

```sh
python -m esptool --chip esp32c3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 deploy/FoloToy-AI-Passport_0x0_all-in-one.bin
```

如果无法连接设备：按住 **BOOT**，短按 **RESET**，松开 **BOOT** 后重试。

## 日常开发

日常更新不需要手动指定偏移，也不应触及 NVS：

```sh
source "$HOME/esp/esp-idf-v5.5.3/export.sh"
idf.py -p /dev/cu.usbmodemXXXX app-flash monitor
```

只有变更分区表、bootloader 或首次迁移时才运行完整 `idf.py flash`。

## 验收 Wi-Fi 与 WSS Passport

1. 第一次烧录后重新完成 Wi-Fi 配网，等待显示 `ONLINE`。
2. 使用受控注册流程将设备凭据写入加密 NVS；设备不会接受或保存 Cloudflare 部署/API Token。
3. 从设备菜单打开 **Kiro**，确认在 NTP 同步后显示 `Relay ready`。
4. 只按 RESET 或断电重启，不运行烧录工具；设备应自动重连 Wi-Fi 和 WSS relay。
5. 用新的 `FoloToy-AI-Passport_0x0_all-in-one.bin` 升级。
6. 确认 Wi-Fi、亮度、时间、加密保存的设备凭据与应用设置仍保留。
