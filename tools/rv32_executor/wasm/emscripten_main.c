/*
 * wasm/emscripten_main.c —— 暴露给前端 JS 的 WASM 边界（骨架）。
 *
 * 前端调用（web/rv32_runner.js）：
 *   load_flash(ptr, len)     — 把 .bin 拷进 8MB 虚拟 flash 并复位
 *   run_steps(n)             — RV32 循环多跑 n 条（当前为外设自测占位）
 *   set_key(mv)              — 写 ADC 按键电压（0/300/595/3300）
 *   get_frame(ptr, cap)      — 拷贝当前 240x320 RGB565 帧，返回 dirty
 *
 * 编译（接入 rv32emu 内核后）：
 *   emcc hooks/esp32c3_vio.c wasm/emscripten_main.c <rv32emu src>... \
 *        -o wasm/esp32c3_wasm.wasm \
 *        -s EXPORTED_FUNCTIONS='_load_flash,_run_steps,_set_key,_get_frame,_malloc,_free'
 *
 * 说明: 8MB flash 用静态全局，超过需改 FLASH_SIZE 或用 malloc。
 */
#include <stdint.h>
#include <string.h>
#include <emscripten/emscripten.h>
#include "../hooks/esp32c3_mmap.h"

static uint8_t s_flash_img[FLASH_SIZE];
static uint8_t s_frame_img[LCD_W * LCD_H * LCD_BPP];

void load_flash(const uint8_t *src, uint32_t len) {
    for (uint32_t i = 0; i < FLASH_SIZE; i++) s_flash_img[i] = 0xFF;
    if (len > FLASH_SIZE) len = FLASH_SIZE;
    for (uint32_t i = 0; i < len; i++) s_flash_img[i] = src[i];
    esp32c3_vio_reset();
    /* 后续: 把 s_flash_img 挂给 rv32emu 的 flash-cache 读取区 */
}

uint32_t run_steps(uint32_t n) {
    /* TODO 接入 rv32emu: while(n--) step();
     *      虚拟外设会触发 esp_write_spi() 从而置 frame_dirty。 */
    extern void esp32c3_vio_tick(void);
    for (uint32_t i = 0; i < n; i++) esp32c3_vio_tick();
    return n;
}

void set_key(int mv) { vio_set_key(mv); }

int get_frame(uint8_t *out, uint32_t cap) {
    return esp32c3_vio_get_frame(out, (int)cap);
}