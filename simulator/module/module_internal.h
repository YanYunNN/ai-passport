/* simulator/module/module_internal.h
 * 固件模块内部接口：不属于 bsp_* 公共 API，仅供模块自身使用。
 */
#pragma once

#include <stdbool.h>

/* ---- bsp_button.c（SDL 按键 → 按键事件） ---- */
void bsp_button_sim_key(int sdl_keycode, bool pressed);
void bsp_button_sim_poll(void);

/* ---- bsp_battery.c（模拟电量） ---- */
void bsp_battery_sim_set(int soc, int mv);
