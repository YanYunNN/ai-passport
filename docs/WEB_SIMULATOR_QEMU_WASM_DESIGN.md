# FoloToy AI Passport —— 在线模拟器真机码执行方案（QEMU esp32c3 → WASM）

> 配套文档：[WEB_SIMULATOR_DESIGN.md](WEB_SIMULATOR_DESIGN.md)（现状与一期设计）、
> [AI_HARDWARE_DEVELOPMENT_GUIDE.md](AI_HARDWARE_DEVELOPMENT_GUIDE.md)（板级硬件事实）。
>
> 本文档回答一个具体问题：**如何在 CF 在线页里把 ESP32-C3 的真实 `.bin`「真正跑起来」**，
> 而不是像当前 [`simulator.ts`](../cloudflare/src/simulator.ts) 那样把 bin 当静态元数据展示。

---

## 0. 问题陈述：为什么现在的"在线烧录"不是真执行

现状 `cloudflare/src/simulator.ts` 的"在线烧录"只做三件事：
1. `/api/simulator/bin-proxy` 跨域拉取 `.bin` 字节流，存到 `state.currentArrayBuffer`；
2. `parseESP32Binary()` 在字节流里**搜索二进制特征串**（`0x32 0x54 0xCD 0xAB`），读出工程名/版本/IDF，填进信息条；
3. `renderScreen()` 用 Canvas **画一段写死的假画面**（"AI Passport - 固件在线运行 87%"），和 bin 内容无关。

**没有任何 RISC-V CPU、内存总线、外设模型**，所以不管加载哪个 `.bin`，屏幕永远是这个静态样板。
文档 `WEB_SIMULATOR_DESIGN.md` 里描述的 "WASM 虚拟内核 / DMA 拦截器" **尚未实现**。
唯一真实的"烧录"是 `btn-web-serial-flash`——那是在浏览器里用 Web Serial + esptool-js **把 bin 写到连在 USB 上的实体板**，不是浏览器内的仿真。

本方案要把缺口补上：让 `.bin` 的**机器码**在云端页面里被真实解译并驱动出画面。

---

## 1. 目标与范围

**目标**：在 `/simulator` 页里，加载 ESP32-C3 的 `.bin` 后，浏览器/云端真正执行其指令，
并通过一块`虚拟屏幕`反映出固件的实际画面（LVGL 菜单 / demo 页面）。

**本轮范围**：技术方案 + 可编译骨架（`chief`），不含完整外设仿真。骨架必须能：
- 在**本地**用 `qemu-system-riscv32 -M esp32c3` 启动真实 `.bin` 并打串口日志，验证"机器码可执行"；
- 为浏览器端预留 **WASM 内核加载通道** 与 **屏幕帧缓冲交换接口**；
- 明确"要让画面真正来自固件"需要做的**外设拦截**清单。

**不做（本轮）**：不承诺在云端跑出完整 ST7789 画面（见 §4 风险）。

---

## 2. 为什么选 QEMU esp32c3 而不是从零写解译器

| 方案 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- |
| 从零写 JS RISC-V 解释器 | 轻量、无依赖 | 需实现 bootloader+ROM 引导优先级+外设+中断+时序，工作量巨大，且固件依赖 ESP-IDF 运行时寄存器 | ✗ 不可行 |
| **QEMU `esp32c3` 机器** | ESP-IDF **官方支持**（`idf.py qemu`），已含 ROM/Flash/基本外设，可跑 bootloader→app、FreeRTOS、UART 日志 | 无 ST7789/LEDC/国 ADC 按键模型（需补 def 屏幕插桩）；WASM 编译见风险 | ✓ **推荐** |

关键证据（已核实）：

- ESP-IDF 5.5.3 自带 `tools/idf_py_actions/qemu_ext.py`，定义 `esp32c3` → `qemu-system-riscv32 -M esp32c3`，串口走
  `socket://localhost:5555`，GDB 走 `localhost:3333`，默认 eFuse 已提供，strap/`boot_mode` 全局参数化，**官方维护**。
- 你板子 BSP 的屏幕是标准路径：`bsp_display_init()` → `esp_lcd`（ST7789 驱动）+ `spi_bus_initialize(SPI2, DMA)`（见 `components/bsp/src/bsp_display.c`）。
  它经由 **SPI2 寄存器 + esp_lcd 缓冲区** 写到 240×320 帧。QEMU esp32c3 没有这块 LCD 设备，所以要在**线上**让它出画面，
  必须在固件 SPI 写入处**拦截数据**（见 §5）。

> 选型依据一句话：**QEMU 已经完成了 90% 的"引导 + 指令执行 + 内存映射 + ROM 外设"** —— 我们只负责
> 补上板级并把它搬进浏览器。自己写解释器等于重新造所有引擎，完全不划算。

---

## 3. 总体架构

```
┌─────────────────────────── CF Worker / Pages ───────────────────────────┐
│  /simulator  (HTML 壳, 现状可用)                                          │
│    ├─ 固件选择: URL 代理 / 拖拽 / 预设             (已实现)              │
│    ├─ wasm 运行内核加载: fetch /qemu/qemu-riscv32.wasm + .js 胶水        │
│    ├─ 虚拟屏幕: Canvas 240x320 ←─ 帧缓冲弹射接口  ← QEMU SPI 拦截        │
│    └─ 串口日志: xterm.js ←─ QEMU UART0← socket                          │
└───────────┬────────────────────────────────────────┴─────────────┐
            │ fetch .bin (字节流)                       │  stdin/stdout/socket
            ▼                                                        ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  QEMU esp32c3 (编译为 WASM —— Emscripten/其他工具链)                      │
│    ● RV32IMC CPU（真实 machine code）                                     │
│    ● 0x0 完整镜像 / 0x10000 app.bin（合并 bootloader + bin + partition）  │
│    ● 外设: UART0 (→串口日志), TICKS, SPI-flash, GPIO                      │
│    ● 【新增】SPI2/esp_lcd 拦截器 → 帧缓冲 (framebuffer) ← 到 Canvas        │
│    ● ADC1 GPIO0 按键梯形 -> 提供虚拟按键电压                              │
└───────────┬───────────────────────────────────────────────┘
            │ (进程内) machine-code 帧数据
            ▼
    QEMU RAM: 0x3FC_8000 起 D-RAM / 0x4200_0000 flash-cache …
```

### 核心思路：**自动拼 Flash 镜像** 喂给真实 ROM

ESP32-C3 的真机 bin 不是裸露的执行实体——它依赖 bootloader → partition table → app 的拼接 + 占用。
QEMU 从 Flash 0x0 启动，需要一份**完整镜像**。ESPTool 的 `--merge-bin` 或模拟器靠拼接：

| 偏移 | 内容 | 来源 |
| --- | --- | --- |
| `0x00000` | bootloader.bin | `idf.py build` 产物（或随官方 demo 合成） |
| `0x08000` | partition-table.bin | 同上 |
| `0x09000` | NVS / OTA（可填充 0xFF） | 默认分区 |
| `0x10000` | **app.bin**（本项目固件 / 社区固件） | 用户加载的 .bin |

模拟器在加载时把"单一 app.bin" 与预设 bootloader/partition 合并成 0x0 镜像，喂给 QEMU。

---

## 4. 本地 POC：先让真实 bin 在 QEMU 里"跑起来"

在投入浏览器之前，先证明**"你的固件在官方 QEMU 里能启动并打日志"**。这是整条路的地基。

### 4.1 安装官方 qemu-riscv32（ESP-IDF 小程序）

```bash
# 进入 ID 环境
source "$HOME/esp/esp-idf-v5.5.3/export.sh"
# 首次会安装 qemu-riscv32
idf.py qemu monitor --target esp32c3
```

### 4.2 手工跑与你 bin 等价的镜像（无硬件时也可脱离 idf.py）

先有好的 `merged.bin`（bootloader+分区+app）：

```bash
# 若已 build 过，直接用 current 镜像构建合并镜
cd build  # 或项目根
python3 -m esptool merge_bin \
    -o merged-firmware.bin \
    --fill-flash-size 4MB \
    0x0 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0x10000 FoloToy-AI-Passport.bin
```

然后：

```bash
qemu-system-riscv32 \
    -machine esp32c3 \
    -nographic \
    -drive file=/path/merged-firmware.bin,if=mtd,format=raw,size=4M \
    -serial stdio
```

> 注意：官方 `idf.py qemu` 会更精确地初始化 eFuse/特性区，短期建议用它而非手写 `-drive`。

### 4.3 预期输出（真实启动日志）

如果 POC 成功，串口会看到 ROM 日志、2nd bootloader、分区校验、ESP-IDF 版本、
以及你的固件 `ESP_LOG`（`bsp_disp`、菜单、`app_main`）。这一步是"机器码真实执行"的直接证据。

**若不成功**，先查：镜像偏移、Flash 大小 id、eFuse/strap 与固件编译配置是否匹配。这一步应被**验证后** commit。

---

## 5. 让画面来自固件：SPI/esp_lcd 帧缓冲拦截

目标从"出日志"到"出画面"，必须在固件写屏处拿到帧数据。基于 `bsp_display.c` + `esp_lcd` 路径：

- 固件往 ST7789 通过 `esp_lcd_panel_draw_bitmap()` 提交 **RGB565 240×240 帧/矩形**，经 SPI2 DMA 写寄存器。
- 在**浏览器侧**，对 QEMU 做一层**内存/外设后门**（内嵌到虚拟机），当固件往保留的帧缓冲地址
  （D-RAM 段，见 `qemu_target.patch.md`）写入那 240×240 区域时，就把它拷贝成 `ImageData`/`Uint16` -> Canvas。

具体机理之一（推荐）：
1. 在 QEMU 里给虚拟板加一个"虚拟 SPI-帧缓冲从机"（QEMU device）device，把一块保留 DRAM 映射为
   **固件可见的"LCD 帧缓冲"**（或让 hal 驱动把写屏直打 host RAM）。
2. QEMU 运行时把该 RAM 段**镜像同步**给 WASM 侧（Emscripten 共享内存 / 每帧回调差量拷贝）。
3. 前端每帧取到 240×240 RGB565，转 RGBA 画到 `<canvas>`。

> 这等于给固件一个"虚拟显存"+一个"帧完成中断"。
> 其余要补的小影外设：按键（GPIO0 ADC 梯形电压）、UART0 日志（已有）；背光 LEDC 可忽略。

---

## 6. 骨架代码（本轮交付）

本轮交付**方案 + 骨架**（非全量实现），放在项目里便于团队接着做。以下是目录与职责：

```
tools/qemu_wasm/                 # 云端运行引擎骨架（新）
  ├── README.md                  # 本骨架编译与接线说明
  ├── pocs/
  │   └── local_run.sh           # M0：本地用官方 QEMU 跑 merged 镜像、出启动日志的证据脚本
  ├── wasm/
  │   └── qemu_target.patch.md   # QEMU 侧加"虚拟 SPI 帧缓冲"设备/挂钩的待实施说明（占位）
  └── web/
      ├── runtime-loader.js      # 前端加载 .wasm 内核的骨架 + 帧回调管道
      └── frame-buffer.js        # RGB565→RGBA 转换 + 渲染一帧到 canvas（替换模拟器假 renderScreen）
```

> 注：`CMakeLists.txt`、`emscripten_shell/` 等编译层条目待 M1 落地时再补，本轮未创建。

`cloudflare/src/simulator.ts` 的集成点（接入时机 = M3/M4 之后）：
- 加载 `.wasm`——.wasm 建议放 **CF Pages 静态资源**（URL 形如 `/qemu/qemu-riscv32.wasm`），
  需保证 MIME 为 `application/wasm`；不要放 Worker 内联（体积与体积扫描限制）。
- 用 `loadWasmRuntime()` 将 `state.currentArrayBuffer` 交给内核启动；用 `renderFrameToCanvas()`
  替换写死的 `renderScreen()`，让画面来自真实固件帧缓冲。

---

## 7. 风险与诚实评估

| 风险 | 级别 | 说明 / 缓解 |
| --- | --- | --- |
| QEMU→WASM 编译链路 | **高** | qemu-system-riscv32 是齐全工具，用 Emscripten 把 `-machine esp32c3` 出 WASM 有一定工程量大
  （系统内存共享、异步、线程）。先建立「本地 qemu 跑通 bin」做地基，再谈 wasm 化。 |
| QEMU esp32c3 LCD 模型缺失 | **中** | 官方模型无 ST7789/LCD。需给固件补"虚拟帧缓冲 + SPI 拦截"（§5），这是一个外设开发块。 |
| 浏览器端 WASM 大小/性能 | 中 | 完整 qemu-system 编译后几个 MB～十几 MB 也是平常；需 gzip/Brotli + lazy 加载。 |
| 固件对 160MHz 时序/中断依赖 | 低 | RV 解释器能按 真实 机器码执行；断点/太快的调度不影响"跑起来"。 |
| 环境可变（CF 静态托管 vs Worker） | 中 | .wasm 需要可访问 URL 与 MIME `application/wasm`；本方案建议放 Pages 静态资源，不放 Worker 内联。 |

> **诚实底线**：本骨架首次可交付的**最可靠里程碑不是"浏览器出完整画面"**，而是——
> **1) 本地用官方 QEMU 唯一地跑通你的真实 bin（出 ROM/ERTiF 日志）；2) 找一个能编译出 wasm esp32c3
> 的链路。** 这两步 pass 之前，"在线画面"都是纸面。建议不 rush"canvas 实时渲染"，先立这两个可验证支柱。

---

## 8. 里程碑建议

- **M0**：本地 `idf.py qemu` 跑通当前固件 bin，串口出启动日志。（地基）
- **M1**：把 M0 的合并镜像流程脚本化（自动 merge_bin + 运行时命令）。
- **M2**：评估并选型 wasm 编译链路，做一个最小 `qemu-system-riscv32 -M esp32c3`（无外设）能在 chrome 里启动、串口能收到日志。
- **M3**：加 SPI/帧缓冲虚拟设备，把 240×240 帧同步到 canvas。
- **M4**：接入当前 `/simulator` UI：用 "wasm 内核" 替代假画面渲染，保留 URL/上传/preset 交互。

---

## 9. 验收

- `idf.py qemu monitor --target esp32c3` 能加载 github / 本仓库 bin 并打印 `app_main` 日志（无 reboot loop/wdt）。
- 浏览器里勾选/拖入同一 bin，wasm 由 QEMU 起，串口日志进 xterm。
- （M3 起）Canvas 出现真实 LVGL 菜单帧，按键/ADC 映射与 `bsp_pins.h` 一致。