# PC 模拟器（P0）

在 Windows / macOS 上直接编译运行 **同一份 `main/` 源码**，用虚拟 BSP 替代真实硬件，
免烧录快速验证菜单、各 demo 页面和游戏交互。

```
main/                     ← 原样编译（固件代码零改动）
components/bsp/include/   ← 复用真实 BSP 头文件（引脚事实来源）
simulator/
  CMakeLists.txt          PC 构建（CMake + FetchContent 拉 LVGL 9.5.0）
  lv_conf.h               LVGL 配置（对齐 sdkconfig.defaults）
  include/                ESP-IDF 垫片头文件（esp_log / freertos / nvs / 外设枚举）
  src/
    main.c                宿主入口：SDL 事件循环 + --autotest
    bsp_*.c               虚拟 BSP：SDL 显示 / 键盘按键 / 音频·电量·I2C 桩
    stubs_*.c             网络·电源模块桩（wifi / kiro / screencast / time_sync / power）
    shim_*.c              垫片实现（日志 / FreeRTOS(pthread) / NVS(内存)）
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

- SDL2 找不到时，用 `-DSDL2_DIR=<SDL2的lib/cmake/SDL2目录>` 或 `-DCMAKE_PREFIX_PATH=<SDL2安装前缀>` 指定。
- LVGL 9.5.0 由 FetchContent 自动下载；无网络或 TLS 受限时，可先手动解压源码后加
  `-DFETCHCONTENT_SOURCE_DIR_LVGL=<lvgl-9.5.0目录>`。
- Windows 运行前需保证 `SDL2.dll`（及 MinGW 运行库）在 exe 旁或 PATH 中。

## 运行

```bash
./simulator/build/ai_passport_sim          # 打开 480x640 模拟窗口（240x320 2x 缩放）
./simulator/build/ai_passport_sim --frames=600   # 渲染 600 帧后退出（冒烟）
./simulator/build/ai_passport_sim --autotest     # 脚本化按键序列：导航/进出页面/长按返回
```

| 按键 | 作用 |
| --- | --- |
| `W` / `↑` | UP（单击=菜单上移；游戏中即时响应） |
| `S` / `↓` | DOWN |
| `Enter` / `Space` | OK（单击=确认；长按≥1s=返回） |
| `Esc` | 退出 |

按键事件语义（PRESS / CLICK / DOUBLE / LONG / HOLD 连发）与固件 button 组件一致，
时长参数集中在 `src/bsp_button.c` 顶部可调。

## P0 覆盖范围

| 模块 | 模拟器行为 | 说明 |
| --- | --- | --- |
| 显示 | LVGL → SDL 窗口，240x320，2x 缩放 | `bsp_display_*` |
| 按键 | 键盘 → 三键事件 + ADC 电压模拟 | `bsp_button_*`，电压窗口取自 `bsp_pins.h` |
| 音频 | 桩：`set_format` 返回不支持 | 音频页显示 format failed，游戏静音运行 |
| 电量 | 模拟值（默认 87%，可调） | `bsp_battery_sim_set(soc, mv)` |
| I2C | 空总线 | `bsp_i2c_*` |
| Wi-Fi / Kiro / 时间同步 / 投屏 | 桩 | 菜单中 Kiro 显示"不可用"（与断网降级一致） |
| 设置持久化 | NVS 为内存版 | 重启模拟器后设置回到默认值 |

## 已知限制（P0）

- **不替代真机验收**：ADC 分压、codec 时序、真实 Wi-Fi 认证、电池标定、浅睡眠功耗等
  硬件行为未模拟，README 的设备验收清单仍需在硬件上执行。
- 时序与 160MHz RISC-V 不同：游戏手感/动画速度会变快，依赖精确帧率的逻辑可能有差异。
- 退出游戏/音频页后，其 worker 线程以低频率空转（P0 简化，不回收），
  频繁进出会积累少量空闲线程，P1 引入真正的任务取消。
- 控制台中文在部分 Windows 终端显示为乱码是代码页问题，不影响程序本身。
  在 PowerShell 里可先 `chcp 65001`。

## P1 方向

- `bsp_audio_write` 接 SDL 音频输出（游戏音效/1kHz 方波真实发声）
- 网络：Wi-Fi 状态机仿真 + Kiro Passport 走宿主 WebSocket 连真实后端
- NVS 落盘到本地文件，设置跨重启保持
- `vTaskDelete` 真正的线程取消，回收空转线程
