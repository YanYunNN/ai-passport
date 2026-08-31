// rv32_runner.js —— 前端加载 WASM 内核并驱动 Blackwell LCD(骨架)
//
// 集成到 cloudflare/src/simulator.ts:
//   1. 替换写死的 renderScreen() —— 改为从 WASM 取真实 240x320 帧。
//   2. 把网页键盘/按钮事件 → wasm.set_key(mv)（ADC 分压档 0/300/595/3300）。
//   3. 加载 .wasm 内核（rv32emu + 外设），把 .bin 拷进 8MB 虚拟 flash。
//
// 本文件是可编译接入点骨架，真实 .wasm 由 rv32_executor/wasm 产出。

export const LCD_W = 240, LCD_H = 320;

/**
 * 由 CLI 加载 .wasm 内核并实例化外部绑定表。
 * 绑定表(WebAssembly.imports) 里要挂的 C 函数，由 emscripten_main.c 导出。
 */
export async function loadEsp32c3Wasm(url, externs) {
    const res  = await fetch(url);
    if (!res.ok) throw new Error(`wasm load ${res.status}`);
    const bytes = await res.arrayBuffer();
    const { instance, module } = await WebAssembly.instantiate(bytes, {
      // external func expected by emscripten glue if any
    });
    // 直接把 Emscripten 的导出翻出来（未用 dlopen）
    return {
      heap: instance.exports.memory?.buffer,
      load_flash: instance.exports.load_flash,   // (ptr,len)
      run_steps:  instance.exports.run_steps,     // (n)
      set_key:    instance.exports.set_key,       // (mv)
      get_frame:  instance.exports.get_frame,     // (outPtr, cap) -> dirty
      malloc:     instance.exports._malloc || instance.exports.malloc,
    };
}

/** 把 Uint8Array(.bin) 拷进 WASM 的 8MB flash。 */
export function flashToWasm(runtime, bin) {
  const ptr = runtime.malloc(bin.length);
  new Uint8Array(runtime.heap.buffer, ptr, bin.length).set(bin);
  runtime.load_flash(ptr, bin.length);
  return ptr;
}

/** 渲染一帧到 240x320 canvas。 */
export function drawFrameFromWasm(runtime, outPtr, outCap) {
  const dirty = runtime.get_frame(outPtr, outCap);
  if (!dirty) return false;
  // outPtr 区域已由 C 填好 240*320*2 字节 RGB565；前端将其转 RGBA → canvas。
  return true;
}