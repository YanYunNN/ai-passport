# rv32_executor —— 浏览器端真机码执行引擎（MVP 骨架）

目标：让 CF `/simulator` 页**真正执行上传的 `.bin` 机器码**并把 ST7789 刷屏渲染到 Canvas、
用网页按键驱动 ADC。全程浏览器端，不依赖任何本机 QEMU。

方案详情见 `docs/WEB_SIMULATOR_RV32EMU_DESIGN.md`。

## 为什么是 rv32emu
RV32IMC 是有条理的、适合走 WASM 的 C 模拟器（~100KB）。见方案 §1 的对比表。

## 目录
```
hooks/
  esp32c3_mmap.h       外设/地址声明(Flash/DRAM/I-RAM/ST7789/ADC)
  esp32c3_vio.c        虚拟板桥: Flash, SPI→帧缓冲, ADC 键
wasm/
  emscripten_main.c    WASM 边界: load_flash/run_steps/set_key/get_frame
web/
  rv32_runner.js       前端: fetch .wasm, 键事件→ADC, 渲染帧
```

## 接入 rv32emu（合金）
1. 拿 rv32emu: https://github.com/sysprog21/rv32emu (MIT)
2. 把 `hooks/esp32c3_vio.c` 与 `wasm/emscripten_main.c` 一起编译进内核;
3. 在 rv32emu 的 **memory load/store** 处接入:
   - flash 读(0x42000000 / 0x40000000 别名) → `s_flash_img`(见 mmap)
   - SPI2 数据寄存器写 → `esp_write_spi(byte)`（重组成帧）
   - GPIO0 ADC 读 → `vio.adc_key_mv`（虚拟分压）
4. Emscripten 构建:
   ```bash
   emcc -I hooks wasm/emscripten_main.c hooks/esp32c3_vio.c <rv32emu src...> \
        -o wasm/esp32c3_wasm.wasm \
        -s EXPORTED_FUNCTIONS='_load_flash,_run_steps,_set_key,_get_frame,_malloc,_free'
   ```

## 集成进 CF `/simulator`
`cloudflare/src/simulator.ts` 当前把 `.bin` 只当元数据：
- 替换 `renderScreen()` 假画面 → 用 `rv32_runner.drawFrameFromWasm()` 取真实帧;
- 把 Fake URL/上传的 `state.currentArrayBuffer` → `flashToWasm()`; 
- 网页 ↑/↓/OK → `set_key(0 / 300 / 595)`（不是 set_gpio_level!）。

## 诚实边界（MVP 未做）
- SPI 时序/ST7789 命令解析当前简化（忽略 CASET/RASET，假定固定 240×320 顺序）；
- 未涉及 IRAM 中断/CPU 频率/唤醒语义——M0 只要求"串口/画面能push"；
- 8MB flash 用静态 buffer，.bin 超过则需 malloc。