# FoloToy AI Passport 烧录说明

此目录包含用于**保留设备设置和 Wi-Fi 凭据**的 ESP32-C3 分段升级包。文件名中的 `0x...` 是必须使用的 Flash 偏移。

| 文件 | Flash 偏移 | 用途 |
| --- | ---: | --- |
| `FoloToy-AI-Passport_0x0_bootloader.bin` | `0x0` | Bootloader |
| `FoloToy-AI-Passport_0x8000_partition-table.bin` | `0x8000` | 分区表 |
| `FoloToy-AI-Passport_0x10000_app.bin` | `0x10000` | 应用程序 |

> 设备的 NVS 位于 `0x9000`–`0xefff`，其中保存 Wi-Fi 凭据、亮度、时间和 Wi-Fi 开关。**正常升级必须跳过这一区域。**

## 命令行烧录

在项目根目录执行。将 `/dev/cu.usbmodemXXXX` 改成实际串口：

```sh
python -m esptool --chip esp32c3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 deploy/FoloToy-AI-Passport_0x0_bootloader.bin \
  0x8000 deploy/FoloToy-AI-Passport_0x8000_partition-table.bin \
  0x10000 deploy/FoloToy-AI-Passport_0x10000_app.bin
```

如果无法连接设备：按住 **BOOT**，短按 **RESET**，松开 **BOOT**，然后重试。

## 图形化烧录工具

选择芯片 **ESP32-C3**，Flash 参数选择 **8 MB / DIO / 80 MHz**，添加下列三行：

| BIN 文件 | 偏移 |
| --- | ---: |
| `FoloToy-AI-Passport_0x0_bootloader.bin` | `0x0` |
| `FoloToy-AI-Passport_0x8000_partition-table.bin` | `0x8000` |
| `FoloToy-AI-Passport_0x10000_app.bin` | `0x10000` |

不要启用 **Erase all**、`erase_flash` 或同类选项；它们会清除 NVS，设备随后需要重新配网。

## 验收 Wi-Fi 持久化

1. 按本目录的三段方式烧录。
2. 完成 Wi-Fi 配网，等待设备显示 `ONLINE`。
3. 只按 RESET 或断电重启；不要再次烧录。
4. 设备应自动重连此前配置的 Wi-Fi。

若要执行工厂清空或恢复出厂，可使用单一连续 raw 镜像；它会清除 NVS，不能用于保留配置的普通升级。
