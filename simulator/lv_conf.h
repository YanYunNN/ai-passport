/* simulator/lv_conf.h
 * LVGL 9.5.0 配置，对齐 sdkconfig.defaults 中固件的 LVGL 选项：
 *   - 16bit RGB565 颜色深度（与 ST7789 面板一致）
 *   - Montserrat 14/20（菜单/状态栏用），RLE 压缩字体（noto_sc 需要）
 *   - SDL 显示驱动（PC 模拟窗口），软件渲染
 *   - TJPGD 图片解码 + MEMFS（与固件一致，供图片页使用）
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* 宿主内存充足，直接用 C 库分配，不模仿 C3 的 32KB 内部池 */
#define LV_MEM_CUSTOM 1
#if LV_MEM_CUSTOM
    #define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
    #define LV_MEM_CUSTOM_ALLOC malloc
    #define LV_MEM_CUSTOM_FREE free
    #define LV_MEM_CUSTOM_REALLOC realloc
#endif

/* 日志：仅告警以上，避免刷屏 */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN

/* 字体 */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_USE_FONT_COMPRESSED 1

/* 图片 */
#define LV_USE_TJPGD 1
#define LV_USE_FS_MEMFS 1
#if LV_USE_FS_MEMFS
    #define LV_FS_MEMFS_LETTER 'M'
#endif

/* SDL 显示驱动（PC 模拟窗口） */
#define LV_USE_SDL 1
#if LV_USE_SDL
    /* find_package(SDL2 CONFIG) 已把 <前缀>/include/SDL2 加入 include 路径，
     * 故直接用 <SDL.h>（Win/macOS/Linux 的 SDL2 CMake 包布局一致）。 */
    #define LV_SDL_INCLUDE_PATH <SDL.h>
#endif

#endif /* LV_CONF_H */
