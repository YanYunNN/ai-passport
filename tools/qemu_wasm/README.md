# QEMU→WASM 在线运行引擎骨架

本目录承载「让 `.bin` 真机码在浏览器里被 QEMU 执行并驱动出画面」的骨架。
配套方案见 `docs/WEB_SIMULATOR_QEMU_WASM_DESIGN.md`。

本轮交付的是**方案 + 骨架**，不是完整外设仿真。先立两个可验证支柱：

> **M0**：本地用官方 QEMU 跑通当前真实 `.bin`（出启动日志）。
> **M1（次）**：wasm 编译链路能加载 `qemu-system-riscv32 -M esp32c3` 并在浏览器里起、串口看到日志。

环境前置：ESP-IDF 5.5.3、ESP-IDF 自带 `qemu-riscv32`。

## 目录
```
pocs/local_run.sh        M0：本地用官方 QEMU 跑真实 bin 的脚本（含 merge_bin）
wasm/qemu_target.patch   待实施的 QEMU 侧帧缓冲 / SPI 外设挂钩说明（占位）
web/runtime-loader.js    前端加载 .wasm 内核 + canvas 帧回调（骨架/接线）
web/frame-buffer.js      RGB565→RGBA 转换，替换 simulator.ts 里的假 renderScreen
```