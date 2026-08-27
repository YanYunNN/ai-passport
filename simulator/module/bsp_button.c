/* simulator/module/bsp_button.c
 * 虚拟按键：键盘 → bsp_btn_ev_t 事件。
 *
 * 对应真实硬件：三个按键共用一个 ADC 引脚（GPIO0），靠分压区分。
 * 宿主直接把键位映射到 BSP 按键，并按 espressif/button 组件的语义
 * 合成 PRESS / CLICK / DOUBLE / LONG / HOLD（连发）事件，供菜单导航
 * 与游戏平滑连发使用。
 *
 * 键位：UP = W / ↑，DOWN = S / ↓，OK = Enter / Space。
 */
#include "bsp_button.h"
#include "bsp_pins.h"
#include "module_internal.h"

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define LONG_PRESS_MS      1000u   /* 长按判定阈值（与 button 组件默认一致） */
#define DOUBLE_CLICK_MS    300u    /* 双击判定窗口 */
#define HOLD_REPEAT_MS     100u    /* 长按后的连发周期（游戏方向平滑移动） */
#define RELEASED_MV        3300    /* 松开时 ADC 上拉电压 */

typedef struct {
    bool pressed;
    uint32_t press_time_ms;
    bool long_fired;
    uint32_t last_hold_ms;
    uint32_t last_click_ms;
} btn_state_t;

static bsp_btn_cb_t s_cb = NULL;
static void *s_user = NULL;
static btn_state_t s_btns[BSP_BTN_COUNT];

static uint32_t now_ms(void)
{
    return (uint32_t)SDL_GetTicks();
}

/* 电压窗口取自 bsp_pins.h 的 BSP_BTN_MV_TABLE（唯一事实来源） */
static const int16_t s_mv_windows[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;

/* ---------------- 公共 API ---------------- */
esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user)
{
    s_cb = cb;
    s_user = user;
    for (size_t i = 0; i < BSP_BTN_COUNT; i++) {
        s_btns[i] = (btn_state_t){0};
    }
    printf("[sim] buttons ready: UP=W/↑ DOWN=S/↓ OK=Enter/Space\n");
    return ESP_OK;
}

int bsp_button_read_mv(void)
{
    for (size_t i = 0; i < BSP_BTN_COUNT; i++) {
        if (s_btns[i].pressed) {
            return (s_mv_windows[i][0] + s_mv_windows[i][1]) / 2;
        }
    }
    return RELEASED_MV;
}

/* ---------------- 宿主事件接口 ---------------- */
static bsp_btn_t key_to_btn(int sdl_keycode)
{
    switch (sdl_keycode) {
    case SDLK_UP:
    case SDLK_w:
        return BSP_BTN_UP;
    case SDLK_DOWN:
    case SDLK_s:
        return BSP_BTN_DOWN;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
        return BSP_BTN_OK;
    default:
        return (bsp_btn_t)-1;
    }
}

void bsp_button_sim_key(int sdl_keycode, bool pressed)
{
    if (!s_cb) return;
    bsp_btn_t btn = key_to_btn(sdl_keycode);
    if (btn == (bsp_btn_t)-1) return;

    btn_state_t *st = &s_btns[btn];
    uint32_t now = now_ms();

    if (pressed) {
        if (st->pressed) return; /* 键盘重复事件由宿主过滤，此处再防一手 */
        st->pressed = true;
        st->press_time_ms = now;
        st->long_fired = false;
        st->last_hold_ms = now;
        s_cb(btn, BSP_BTN_PRESS, s_user);
    } else {
        if (!st->pressed) return;
        st->pressed = false;
        uint32_t held = now - st->press_time_ms;
        if (held < LONG_PRESS_MS) {
            if (st->last_click_ms != 0 && now - st->last_click_ms <= DOUBLE_CLICK_MS) {
                s_cb(btn, BSP_BTN_DOUBLE, s_user);
                st->last_click_ms = 0;
            } else {
                s_cb(btn, BSP_BTN_CLICK, s_user);
                st->last_click_ms = now;
            }
        }
    }
}

void bsp_button_sim_poll(void)
{
    if (!s_cb) return;
    uint32_t now = now_ms();

    for (size_t i = 0; i < BSP_BTN_COUNT; i++) {
        btn_state_t *st = &s_btns[i];
        if (!st->pressed) continue;

        if (!st->long_fired) {
            if (now - st->press_time_ms >= LONG_PRESS_MS) {
                st->long_fired = true;
                st->last_hold_ms = now;
                s_cb((bsp_btn_t)i, BSP_BTN_LONG, s_user);
            }
        } else if (now - st->last_hold_ms >= HOLD_REPEAT_MS) {
            st->last_hold_ms = now;
            s_cb((bsp_btn_t)i, BSP_BTN_HOLD, s_user);
        }
    }
}
