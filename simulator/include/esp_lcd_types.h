/* simulator/include/esp_lcd_types.h —— 宿主垫片，仅供 bsp_display.h 编译通过 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct esp_lcd_panel_t;
struct esp_lcd_panel_io_t;

typedef struct esp_lcd_panel_t *esp_lcd_panel_handle_t;
typedef struct esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;

#ifdef __cplusplus
}
#endif
