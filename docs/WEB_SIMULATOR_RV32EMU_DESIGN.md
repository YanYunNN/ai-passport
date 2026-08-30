# FoloToy AI Passport —— 在线模拟器真机码执行方案（rv32emu → WASM）

> 配套：[WEB_SIMULATOR_DESIGN.md](WEB_SIMULATOR_DESIGN.md)（现状）、[AI_HARDWARE_DEVELOPMENT_GUIDE.md](AI_HARDWARE_DEVELOPMENT_GUIDE.md)（板知识）。
>
> 目的：让 CF `/simulator` 页**真正执行上传的 ESP32-C3 `.bin` 机器码**，并把固件刷屏**真实渲染到 Canvas**、
> 由网页按键**驱动 ADC 采样**。本方案**不依赖任何本机 QEMU**，全程前端可用 —— 选型见 §2。

---

## 1. 为什么放弃 QEMU，改选 rv32emu

上一版方案（`WEB_SIMULATOR_QEMU_WASM_DESIGN.md`）选 QEMU，但它不适合"CF 页面在用户浏览器端跑"：

| | QEMU (`qemu-system-riscv32`) | rv32emu |
|---|---|---|
| 形态 | 完整 SoC，仿真能力强 | 极简 RV32IMC 解释器 |
| 编 WASM | 大（几 MB~几十 MB）、编译复杂、线程/异步难题 | **~100KB，纯 C 可直编 WASM** |
| 移动（浏览器） | 重，遭遇 线程/SharedBuffer/加载 难题多 | **轻，直接 fetch 即用** |
| 你的场景 | 杀鸡用牛刀，且上不了"点击即玩" | **匹配**（上传 bin→执行） |

**结论**：做一个"浏览器里就能跑的 RV32 引擎"最现实——用游 `rv32emu`（SysProg-Lab，C，RV32IMC，~100KB WASM）做 CPU 核心，配一块"虚拟外设"（虚拟 Flash + ST7789 SPI→帧缓冲 + ADC 三键）。全部跑在前端，不碰本机。

---

## 2. 你的固件写屏的真实路径（决定怎么拦截）

从 `components/bsp/include/bsp_pins.h` 与 `components/bsp/src/bsp_display.c` 确认：

- 屏幕：**ST7789P3 240×320, 4-line SPI**（`SPI2_HOST`, MOSI=9, SCLK=8, CS=1, DC=20）。
- 写屏：`esp_lcd_panel_draw_bitmap()` → **`esp_lcd` / SPI2 控制器** → ST7789；**不是直接对 D-RAM 内存块写**。

因此，"抓到帧"不是"DMA hook 一处写完 240×320"，而是一个 **SPI 事务级拦截**：
固件把 RGB565 像素和屏幕命令（CASET/RASET/RAMWR……）**串行写到 SPI 寄存器**。

**所以 rv32emu 里的做法**：为 SPI2 提供一个小型**虚拟主机模型**，把 ST7789 当成一块"吞 SPI 字节的显存"：
- 收到 `0x2C`（RAMWR）后收集后续 240×320×2 字节 RGB565；
- 一帧收完→触发一次`updateDisplay(RGB565buf)` → 前端 Canvas 渲染。
- 收命令时解析 CASET/RASET/MADCTL（可选，MVP 可先假定约定 240×320 顺序）。

这就是"为板子控制器配一个虚拟 ST7789 + 显存"，是**唯一的 N 拦路虎**（别的都简单）。

### 键盘的事实：是 ADC 分压，不是 IO 直连

`bsp_pins.h`：三键共用 **GPIO0 / ADC1_CH0 一个 10k 上拉分压梯**：

| 键 | 分压 | ADC 读数(mV) |
|---|---|---|
| 上 UP | 0Ω | 0 |
| 下 DOWN | 1kΩ | ~300 |
| 确定 OK | 2.2kΩ | ~595 |
| 松开 | — | ~3300 (上拉) |

→ 不是 `setGpioLevel(pin, high)` 的每键一个 IO，而是**读 GPIO0 的 ADC 电压**选档。
MVP 模拟：网页按键 → 设一个"soc电压"值 → CPU 读 `GPIO0 ADC（ADC_UNIT1）` 时返回对应mV。

---

## 3. 总体架构

```
┌────────── 前端页面 ──────────────┐
│   <input type=file> .bin         │
│   <canvas id=screen 240x320>     │
│   ↑/↓/OK 按钮 + 键盘            │
└────────────┬─────────────────────┘
             │ (1) 读文件 → 8MB 虚拟 Flash ArrayBuffer
             ▼
┌───────────────────────────────────────┐
│  rv32emu (WASM) — RV32IMC CPU          │
│   ● PC/x30 运行 loop                    │
│   ● 内存模型:                           │
│     - 0x00000000 保留/外设映射          │
│     - 0x3FC80000 D-RAM                  │
│     - 0x42000000 flash-map (cache)      │
│   ● 外设 hook（C 里实现）:              │
│     - 虚拟 Flash(8MB)                 │
│     - 虚拟 SPI2 + ST7789 → framebuffer │
│     - 虚拟 ADC(按键电压)               │
│     - UART0 → console(可选)            │
└──────────┬──────────────────────────────┘
           │ (3) 写屏事务完成 → 0x… 回调
           ▼
      updateFrame(rgb565) → Canvas
```

---

## 4. 里程碑（建议按此推进）

| 阶段 | 交付 | 验收 |
| --- | --- | --- |
| **M0** 上传 + rv32 执行 | 8MB 虚拟 flash，把 .bin 载入 0x0，CPU loop 跑，能看串口日志 | 网页里加载 bin 后 console 出现固件启动 ESP_LOG |
| **M1** 屏幕 | 虚拟 SPI2/ST7785 → 帧缓冲 → Canvas | 菜单/首屏呈现在 Canvas（**这是难度核心**） |
| **M2** 按键 | ADC 按键电压映射，按钮/键盘案件 | 按下 UP/DOWN/OK 能让 LVGL 菜单移动/进入 |
| **M3** 打磨 | 中文界面、预设固件、进度、错误处理 | CF 端整体可用 |

---

## 5. 待确认的技术依赖（诚实标注）

- **rv32emu 源码与 WASM 集成**：需把 C 内核中关于 memory 的 `load/store` 改成"带地址回调"，触发外设。应验证它能编成 WASM 且可加载 `.wasm` 镜像（Emscripten）。
- **SPI 语义细节**：dummy `esp_lcd` 驱动实际使用的寄存器/时序须结合 `bsp_display.c` 核对；MVP 可先认"接到 RAMWR 后读连续 2 字节/px"的简化路径，再校准。
- **IRAM/向量**：固件用到 IRAM loader（蹦到 `xtensa`?），rv32emu 需正确处理中断向量/CACHE——先以"串口能跑起来"为 M0 验收，IRAM 细节后。

---

## 6. MVP 代码骨架（本目录）

```
tools/rv32_executor/          ← 全部为此方案新增
  README.md                    本方案说明 + 编译/接入命令
  hooks/esp32c3_mmap.h         Flash/DRAM/I-RAM/ST7789/ADC 地址与状态声明
  hooks/esp32c3_vio.c          虚拟板桥: Flash + SPI→帧缓冲 + ADC 键 + UART 占位
  wasm/emscripten_main.c       WASM 边界: load_flash/run_steps/set_key/get_frame
  web/rv32_runner.js           前端: 加载 .wasm, 按键→ADC 电压, 帧回调→Canvas
```