/* simulator/src/sim_internal.h
 * 宿主侧内部接口：不属于 bsp_* 公共 API，仅供 simulator 自身使用。
 */
#pragma once

#include "bsp_button.h"
#include <stdbool.h>

/* ---- bsp_button.c（SDL 按键 → 按键事件） ---- */
/* 由宿主主循环在 SDL_KEYDOWN/UP 时调用 */
void bsp_button_sim_key(int sdl_keycode, bool pressed);
/* 由宿主主循环每帧调用，推进 LONG/HOLD(连发) 状态机 */
void bsp_button_sim_poll(void);

/* ---- bsp_battery.c（模拟电量） ---- */
/* 设置模拟电量（SOC 0..100、电压 mV），便于测试低电量 UI */
void bsp_battery_sim_set(int soc, int mv);
