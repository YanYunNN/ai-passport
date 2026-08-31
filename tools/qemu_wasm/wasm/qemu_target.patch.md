# QEMU 侧 SPI / 帧缓冲虚拟设备挂点（占位）

要让「固件画面真实出到 canvas」，需要在 QEMU `esp32c3` 机器里为
`bsp_display.c`（`esp_lcd` ST7789 → SPI2 DMA，RGB565 240×240）补一个**虚拟帧缓冲从机**。
本文件是**待实施说明**，不是可应用补丁——待 M0/M1 地基立住后再补实际代码。

## 思路：给固件一块「虚拟显存」+「帧完成中断」

固件写屏的路径（`components/bsp/src/bsp_display.c`）：

```
esp_lcd_panel_draw_bitmap() → SPI2 寄存器写 → ST7789
```

要在浏览器拿到帧，不实现 ST7789 寄存器细节，而是把这块离线"重定向"到一块**宿主可见的 RAM（帧缓冲）**：

1. 在 QEMU 机器里保留一段 D-RAM（如 `0x3FC_D0000` 附近）作为「虚拟 LCD 帧缓冲」区域；
2. 给你固件的**自定义 SPI LCD host**（或 hal `frame_buffer_*`）映射到该段, 写屏实际写到这段;
3. 帧写完置一个"帧完成"标志（可给某 GPIO / 中断寄存器），QEMU 据此把这段 RAM **差量同步**给 WASM 侧;
4. WASM 侧 `frame-buffer.js` 拿到 240×240 RGB565, 转 RGBA, 推到 `<canvas>`。

这等价于：模拟器给固件一个"写了就会出画面的显存 + 帧同步中断"，而不是笨重地完整模拟 ST7789 时序。

## 具体待做事项（写补丁前的检查清单）

- [ ] 确认 QEMU 源码树被加入本项目（以 submodule / vendored 形式），或记录外部 fork 版本。
- [ ] 在 `hw/` 加一个 `esp32_ai_lcd` 精简设备：记录 `0x3FC8_xxxx` 帧缓冲段 + `frame_complete` 中断。
- [ ] ESP-IDF hal spi / esp_lcd 驱动在 QEMU 下的行为确认（哪些寄存器必须为 dup, 避免后退）。
- [ ] 决定同步方式：`SharedArrayBuffer`（线程/worker 双缓存）还是每帧 `postMessage` 差量拷贝。

## 不做的（矛盾点澄清）
- QEMU 官方 esp32c3 没有 ST7789/SPI-LCD, 也不会有; 我们**不去补齐 2nd 外设**, 只拦截能出画面那一条。
- 「让固件相信它写的是 ST7789」和「让虚拟显存被 host 读到」是两个可分离目标：本轮先做后者。

## 接入点标记（供后续 patch）
- SPI2 主机寄存器区间 / esp_lcd DMA 基址 → QEMU `esp32` 类外设（待具体偏移核对，勿脑补）。
- 提请：所有具体区间必须参 `components/bsp/include/bsp_pins.h` 与 `bsp_display.c`, 不得套别的 ESP32-C3 板。