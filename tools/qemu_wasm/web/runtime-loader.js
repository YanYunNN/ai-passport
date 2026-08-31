// runtime-loader.js — 前端加载 QEMU→WASM 内核的骨架（接线层）
//
// 目标: 把 `state.currentArrayBuffer`(ESP32-C3 .bin) 交给 WASM 里的 QEMU esp32c3 执行,
//       并从共享帧缓冲渲染 240×320 到 `<canvas>`。
//
// 本轮占位: 函数签名 + 加载通道 + 帧回调管道已经立好; 实际 .wasm 编译产物 await M0/M1。

// ---- 常量（与 bsp_pins.h / bsp_display.c 对齐；不要臆改） ----
export const LCD_W = 240;
export const LCD_H = 320;
export const LCD_BPP = 16; // RGB565

// 帧缓冲在 D-RAM 里的保留段——QEMU 侧 esp32_ai_lcd 设备会把这段的写同步出来(见 qemu_target.patch.md)
// ⚠️ 本地址为占位, 必须与 QEMU 设备补丁一致, 且应符合 bsp 实际布局; 勿用别的开发板数值。
export const FRAME_BUF_DRAM = 0x3FC8_0000;
export const FRAME_BUF_LEN = LCD_W * LCD_H * (LCD_BPP / 8); // 153600 bytes

/**
 * 加载 WASM 运行内核（QEMU->WASM）。当前为骨架：
 * 返回一个 resolve 后在浏览器里可 getInstance 的 Runtime 对象。
 * 真实实现需保证 MIME 为 application/wasm 且网络可及（建议放 CF Pages 静态资源, 不放 Worker 内联）。
 */
export async function loadWasmRuntime(url) {
    if (typeof WebAssembly === "undefined") throw new Error("浏览器不支持 WebAssembly");

    // --- 待实现(M1): 真正的 QEMU WASM 加载 ---
    // 1) fetch(url) → instance
    // 2) 实例化时把 C 侧的 memory 取回; 若用线程/SharedArrayBuffer, 需设置 COOP/COEP 头
    // 3) 随后把 ArrayBuffer 传给 QEMU 的 flash 加载入口
    const res = await fetch(url);
    if (!res.ok) throw new Error(`wasm 加载失败 HTTP ${res.status}`);
    const bytes = await res.arrayBuffer();
    const { instance } = await WebAssembly.instantiate(bytes, {
      env: {
        // esp_log 输出 → 前端 xterm（占位: 由 simulator.ts 提供 logSerial 回调）
        _log_uart: (ptr, len) => {},
      },
      wasi_snapshot_preview1: {},
    });
    return { instance, export: instance.exports, startFlash(start, buf) { /* 见 M1 pad */ } };
  }

/** 取一帧虚拟显存并画到指定 2D context（替换 simulator.ts 里写死的 renderScreen 假画面）。 */
export function pumpFrame(canvas, memoryView /* , offset = 0 */) {
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  // 占位：真实实现从 QEMU 同步好的 RGB565 buffer 读, 转 RGBA。
  // this.pump 由 M3 (帧缓冲同步) 提供。
}