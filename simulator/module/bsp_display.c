/* simulator/module/bsp_display.c
 * 虚拟显示：LVGL 渲染到 SDL 窗口（240x320，默认 1 倍缩放，即 240x320）。
 * 对应真实硬件：ST7789P3 240x320 SPI 屏 + LEDC 背光。
 */
#include "bsp_display.h"
#include "bsp_pins.h"

#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"

#include <stdio.h>
#include <stdlib.h>

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

void bsp_display_sleep(void)
{
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
    /* 窗口缩放：默认 1 倍（240x320，与真实屏幕 1:1）；可用环境变量 SIM_ZOOM 调整，
     * 如 SIM_ZOOM=2 得到 480x640。 */
    float zoom = 1.0f;
    const char *env_zoom = getenv("SIM_ZOOM");
    if (env_zoom && env_zoom[0]) {
        float parsed = (float)atof(env_zoom);
        if (parsed >= 0.5f && parsed <= 4.0f) {
            zoom = parsed;
        } else {
            printf("[sim] 忽略无效 SIM_ZOOM=%s（有效范围 0.5~4.0）\n", env_zoom);
        }
    }
    lv_sdl_window_set_zoom(s_disp, zoom);
    printf("[sim] LVGL display %dx%d (zoom %.1fx → 窗口 %dx%d)\n",
           BSP_LCD_W, BSP_LCD_H, (double)zoom,
           (int)(BSP_LCD_W * zoom), (int)(BSP_LCD_H * zoom));
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
