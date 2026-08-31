/*
 * esp32c3_vio.c —— 给 rv32emu 的虚拟板载体（骨架）。
 *
 * 暴露给 WASM 边界的符号:
 *   vio_get() / esp32c3_vio_reset() / vio_set_key(mv) / esp32c3_vio_get_frame()
 *   esp_write_spi(byte)  —— 固件写 SPI 数据寄存器时，解释器调用此桥。
 *
 * MVP 目标: 不是精确外设仿真，而是提供"能接上 rv32emu load/store hook" +
 *   把 ST7789 刷屏字节流重组成 RGB565 帧给 Canvas 的最短路径。
 * 真实 SPI 命令/时序需对照 bsp 驱动校准（见 README 与 design doc）。
 */
#include "esp32c3_mmap.h"
#include <string.h>

static uint8_t s_flash_buf[FLASH_SIZE];
static esp32c3_vio_t s_vio = {
    .flash = s_flash_buf,
    .flash_size = FLASH_SIZE,
    .adc_key_mv = 3300,
};

esp32c3_vio_t *vio_get(void) { return &s_vio; }

void vio_set_key(int mv) { s_vio.adc_key_mv = mv; }

void esp32c3_vio_reset(void) {
    memset(s_vio.framebuffer, 0, sizeof(s_vio.framebuffer));
    s_vio.spi_active   = 0;
    s_vio.raster_count = 0;
    s_vio.frame_dirty  = false;
}

/* 简化 ST7789: 收一字节, 识别 RAMWR(0x2C) 后持续收数据。 */
static void st7789_spi_byte(esp32c3_vio_t *v, uint8_t byte) {
    if (!v->spi_active) {
        /* 命令字节; MVP 只认 RAMWR 开头(其余命令忽略) */
        if (byte == 0x2C) {
            v->spi_active   = 1;
            v->raster_count = 0;
        }
        return;
    }
    if (v->raster_count < sizeof(v->framebuffer)) {
        v->framebuffer[v->raster_count++] = byte;
    }
    if (v->raster_count >= sizeof(v->framebuffer)) {
        v->spi_active  = 0;
        v->frame_dirty = true;   /* 一帧收满 */
    }
}

/* 解释器在固件写 SPI 数据寄存器时调用。reg 参数 MVP 忽略。 */
void esp_write_spi(uint32_t reg, uint8_t byte) {
    (void)reg;
    st7789_spi_byte(&s_vio, byte);
}

/* 供模拟器主循环 tick 使用：MVP 无实际指令执行，仅推进(留空逻辑)。 */
void esp32c3_vio_tick(void) {
    /* TODO: 接入 rv32emu 后，把"解释器在本 step 内对 SPI/ADC/Flash 的访问"
     *       路由到上面的 esp_write_spi / flash 读取。 */
}

int esp32c3_vio_get_frame(uint8_t *out, int cap) {
    esp32c3_vio_t *v = &s_vio;
    int n = (int)sizeof(v->framebuffer);
    if (out && cap >= n) memcpy(out, v->framebuffer, (size_t)n);
    return v->frame_dirty ? 1 : 0;   /* 调用方拿到 dirty 后应清 */
}