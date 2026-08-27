/* simulator/src/bsp_display.c
 * 虚拟显示：LVGL 渲染到 SDL 窗口（240x320，2 倍缩放）。
 * 对应真实硬件：ST7789P3 240x320 SPI 屏 + LEDC 背光。
 */
#include "bsp_display.h"
#include "bsp_pins.h"

#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"

#include <stdio.h>

static lv_display_t *s_disp;

esp_err_t bsp_display_init(void)
{
    /* SDL 窗口与 LVGL 显示在 bsp_lvgl_init() 中创建 */
    return ESP_OK;
}

esp_lcd_panel_handle_t bsp_display_panel(void)
{
    return NULL;
}

esp_lcd_panel_io_handle_t bsp_display_io(void)
{
    return NULL;
}

void bsp_display_backlight(uint8_t percent)
{
    /* 桌面窗口无背光；0% 对应固件熄屏语义，宿主忽略 */
    (void)percent;
}

lv_display_t *bsp_lvgl_init(void)
{
    if (s_disp) return s_disp;
    if (!lv_is_initialized()) {
        lv_init();
    }

    /* lv_sdl_window_create 内部完成 SDL_Init(SDL_INIT_VIDEO) 与 lv_tick_set_cb */
    s_disp = lv_sdl_window_create(BSP_LCD_W, BSP_LCD_H);
    if (!s_disp) {
        fprintf(stderr, "[sim] lv_sdl_window_create failed\n");
        return NULL;
    }
    /* 2 倍缩放：240x320 → 480x640 窗口，屏幕更大更易用 */
    lv_sdl_window_set_zoom(s_disp, 2.0f);
    printf("[sim] LVGL display %dx%d (zoom 2x)\n", BSP_LCD_W, BSP_LCD_H);
    return s_disp;
}

bool bsp_lvgl_lock(int timeout_ms)
{
    /* 宿主为单线程 LVGL 主循环，无需加锁 */
    (void)timeout_ms;
    return true;
}

void bsp_lvgl_unlock(void)
{
}
