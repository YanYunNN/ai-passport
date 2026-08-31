/*
 * esp32c3_mmap.h —— rv32emu 的内存钩子与外设声明（骨架）
 *
 * 对应项目真实事实（components/bsp/include/bsp_pins.h）:
 *   - Flash 8MB，启动在 0x0，代码经 flash-cache 0x4200_0000 等别名访问
 *   - D-RAM ~0x3FC00000 区段、I-RAM ~0x40380000
 *   - 屏: ST7789P3 240x320 4-line SPI (SPI2_HOST)，**不是** D-RAM framebuffer
 *   - 键: 三键共用 GPIO0 / ADC1_CH0 分压梯(0/300/595mV，松开3300)
 *
 * 地址值是 MVP 占位，粒度必须对照真实 bsp/AI_HARDWARE_DEVELOPMENT_GUIDE 校准。
 */
#ifndef ESP32C3_MMAP_H
#define ESP32C3_MMAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FLASH_SIZE             (8u * 1024u * 1024u)  /* 8MB */
#define LCD_W 240
#define LCD_H 320
#define LCD_BPP 2             /* RGB565 */

/* ---- 虚拟外设状态机 ----
 * 每帧固件刷屏 = RAMWR(0x2C) 后续收 240*320*2 字节 RGB565。
 */
typedef struct esp32c3_vio {
    uint8_t *flash;               /* 8MB 虚拟 flash */
    size_t   flash_size;

    uint8_t  framebuffer[LCD_W * LCD_H * LCD_BPP]; /* RGB565 */
    bool     frame_dirty;         /* 有新帧待前端取 */

    int      spi_active;          /* 正在收 RAMWR 数据 */
    uint32_t raster_count;        /* 已收字节计数 */

    int      adc_key_mv;          /* 按键分压读数 */
} esp32c3_vio_t;

/* 由实现文件(esp32c3_vio.c)提供 */
esp32c3_vio_t *vio_get(void);
void esp32c3_vio_reset(void);
void vio_set_key(int mv);
int  esp32c3_vio_get_frame(uint8_t *out, int cap); /* 返回 dirty */

/* 解释器接入点 */
void esp_write_spi(uint32_t reg, uint8_t byte); /* 固件写 SPI 数据寄存器 */
void esp32c3_vio_tick(void);                    /* 模拟器主循环逐 tick */

#endif