# PC 模拟器（通用外壳 + 可插拔固件模块）

在 Windows / macOS 上运行 **同一份 `main/` 源码**的固件模块，免烧录快速验证菜单、各 demo 页面和游戏交互。

架构分两层，通过固定 ABI 解耦：

```
sim_shell（通用外壳，零固件代码）
  ├─ SDL 窗口与事件泵
  ├─ 固件模块加载器（扫描/加载 firmware/ 下的 .dll/.so/.dylib）
  └─ 服务表实现：日志 / FreeRTOS(pthread) / NVS(内存)   ← sim_api_t
        ▲
        │ 唯一交互契约（simulator/include/sim_api.h）
        ▼
sim_firmware（固件模块，可插拔）
  ├─ main/ 源码原样编译（app_main、菜单、全部 demo 页）
  ├─ 模块侧宿主适配：bsp_display(SDL 窗口) / bsp_button(键盘) / 音频·电量·I2C 桩
  └─ ABI 胶水：垫片函数（esp_log/freertos/nvs）转调外壳服务表
```

**可插拔**：换固件 = 把新的固件模块文件放进 `firmware/` 目录（或 `--firmware=` 指定），外壳与固件都不需要重新编译。ABI 版本（`SIM_API_VERSION` / `SIM_FIRMWARE_ABI_VERSION`）不匹配时加载会被拒绝并提示。

## 目录结构

```
simulator/
  CMakeLists.txt         构建两个目标：sim_shell + sim_firmware
  lv_conf.h              LVGL 配置（编译进固件模块）
  include/               ESP-IDF 垫片头文件 + sim_api.h（ABI 契约）
  shell/                 通用外壳：加载器、事件循环、autotest、服务实现
  module/                固件模块：ABI 胶水 + 宿主 BSP + 网络/电源桩
  firmware/              构建产物输出目录（固件模块，.gitignore 排除）
```

## 环境准备

- CMake ≥ 3.16（建议 ≥ 3.20）、Ninja（可选）、C11 编译器（GCC 或 Clang，不支持 MSVC：
  `main/` 使用了 `__attribute__((packed))`）
- SDL2 开发库（2.30.x）

**Windows（MinGW-w64）**：任选其一

- MSYS2：`pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,SDL2}`
- WinLibs（便携版 GCC）+ SDL2 mingw 开发包（`SDL2-devel-*-mingw.tar.gz`）

**macOS**：

```bash
brew install cmake ninja sdl2
```

## 构建

```bash
# Windows（MSYS2 内）或 macOS：
cmake -S simulator -B simulator/build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build simulator/build
```

- 产出：`build/sim_shell.exe`（外壳）+ `firmware/sim_firmware.dll`（固件模块）
- SDL2 找不到时，用 `-DSDL2_DIR=<SDL2的lib/cmake/SDL2目录>` 或 `-DCMAKE_PREFIX_PATH=<SDL2安装前缀>` 指定。
- LVGL 9.5.0 由 FetchContent 自动下载；无网络或 TLS 受限时，可先手动解压源码后加
  `-DFETCHCONTENT_SOURCE_DIR_LVGL=<lvgl-9.5.0目录>`。
- Windows 运行前需保证 `SDL2.dll`（及 MinGW 运行库）在 exe 旁或 PATH 中。

## 运行

```bash
./simulator/build/sim_shell                      # 自动加载 firmware/ 下的固件模块
./simulator/build/sim_shell --firmware=/path/to/sim_firmware.dll   # 指定固件
./simulator/build/sim_shell --frames=600         # 渲染 600 帧后退出（冒烟）
./simulator/build/sim_shell --autotest           # 脚本化按键序列：导航/进出页面/长按返回
```

固件模块查找顺序：`--firmware` 参数 > 环境变量 `SIM_FIRMWARE_DIR` > exe 旁/上级的 `firmware/` > 当前目录 `firmware/`。

窗口大小：默认 240x320（与真实屏幕 1:1）。想放大窗口可用环境变量
`SIM_ZOOM=1.5`（有效范围 0.5~4.0），例如 `SIM_ZOOM=2` 得到 480x640。

| 按键 | 作用 |
| --- | --- |
| `W` / `↑` | UP（单击=菜单上移；游戏中即时响应） |
| `S` / `↓` | DOWN |
| `Enter` / `Space` | OK（单击=确认；长按≥1s=返回） |
| `Esc` | 退出 |

按键事件语义（PRESS / CLICK / DOUBLE / LONG / HOLD 连发）与固件 button 组件一致，
时长参数集中在 `module/bsp_button.c` 顶部可调。

## 编写/接入另一个固件模块

1. 实现 `sim_firmware_load()`（见 `include/sim_api.h` 的 ABI 契约）；
2. 模块内的 ESP-IDF 垫片函数一律转调 `sim_api_t` 服务表（参考 `module/module_entry.c`）；
3. 编译为共享库放入 `firmware/`（或 `--firmware=` 指定）即可被外壳加载。

## P0 覆盖范围

| 模块 | 模拟器行为 | 说明 |
| --- | --- | --- |
| 显示 | LVGL → SDL 窗口，240x320（默认 1:1；`SIM_ZOOM` 可调） | `bsp_display_*` |
| 按键 | 键盘 → 三键事件 + ADC 电压模拟 | `bsp_button_*`，电压窗口取自 `bsp_pins.h` |
| 音频 | 桩：`set_format` 返回不支持 | 音频页显示 format failed，游戏静音运行 |
| 电量 | 模拟值（默认 87%，可调） | `bsp_battery_sim_set(soc, mv)` |
| I2C | 空总线 | `bsp_i2c_*` |
| Wi-Fi / Kiro / 时间同步 / 投屏 | 桩 | 菜单中 Kiro 显示"不可用"（与断网降级一致） |
| 设置持久化 | NVS 为内存版（外壳持有） | 重启模拟器后设置回到默认值 |

## 已知限制（P0）

- **不替代真机验收**：ADC 分压、codec 时序、真实 Wi-Fi 认证、电池标定、浅睡眠功耗等
  硬件行为未模拟，README 的设备验收清单仍需在硬件上执行。
- **网络未打通**：Kiro Passport 页显示"不可用"。打通方案已评估（真实 WebSocket/HTTPS 传输层
  替换 + 真实审批状态机编译进模块），属 P2 范畴。
- 时序与 160MHz RISC-V 不同：游戏手感/动画速度会变快，依赖精确帧率的逻辑可能有差异。
- 退出游戏/音频页后，其 worker 线程以低频率空转（P0 简化，不回收），
  频繁进出会积累少量空闲线程，P1 引入真正的任务取消。
- 控制台中文在部分 Windows 终端显示为乱码是代码页问题，不影响程序本身。
  在 PowerShell 里可先 `chcp 65001`。

## 路线图

- P1：SDL 音频输出（游戏音效/1kHz 方波真实发声）；`vTaskDelete` 真正的线程取消；NVS 落盘
- P2：网络打通——真实 kiro_passport.c 状态机 + 宿主 WebSocket/HTTPS 传输层，Kiro 全流程可用
- 可选：QEMU 加载后端（跑真机 bin，官方 `idf.py qemu` 已有 CPU/虚拟屏/串口支持，
  但无板级外设与 WiFi 模型，与"打通网络"目标互斥，适合启动/镜像级验证）
