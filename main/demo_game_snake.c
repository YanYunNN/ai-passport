// main/demo_game_snake.c - 《贪吃蛇》(Snake)
// 三键方案: 上/下 = 左转/右转(按下即响应, 低延迟), OK = 暂停/继续。长按 OK 由 main 拦截返回。
// 内存约束: 蛇身对象数 = 蛇长(吃食物才新增一个对象, 不吃时复用尾巴), 与泡泡龙量级相当。
#include "demo_game_snake.h"
#include "game_audio.h"
#include "ui_system.h"
#include "ui_font_noto_sc_14.h"
#include "bsp_button.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS            18
#define ROWS            21
#define CELL            12      // 网格尺寸 12px (18 * 12 = 216px, 21 * 12 = 252px)
#define BOARD_LEFT      12
#define BOARD_TOP       34
#define MAX_SEG         (COLS * ROWS)
#define TICK_PERIOD_MS  20

typedef struct {
    int16_t x, y;
} seg_t;

// 方向: 0上 1右 2下 3左
static const int DX[4] = { 0, 1, 0, -1 };
static const int DY[4] = { -1, 0, 1, 0 };

static lv_obj_t *s_scr;
static lv_obj_t *s_score_label;
static lv_obj_t *s_high_label;
static lv_obj_t *s_pause_label;
static lv_obj_t *s_food_obj;
static lv_obj_t *s_food_shine;
static lv_obj_t *s_food_leaf;
static lv_obj_t *s_game_over_panel;
static lv_obj_t *s_over_title_label;
static lv_obj_t *s_over_score_label;
static lv_timer_t *s_timer;

// 蛇头圆润眼睛
static lv_obj_t *s_head_eye1;
static lv_obj_t *s_head_glint1;
static lv_obj_t *s_head_eye2;
static lv_obj_t *s_head_glint2;

static seg_t s_snake[MAX_SEG];
static lv_obj_t *s_seg_objs[MAX_SEG];
static seg_t s_food;
static int s_len;
static int s_dir;
static int s_turn_queue[2];
static int s_turn_queue_len;
static int s_score;
static int s_high_score;
static int s_move_accum;
static bool s_paused;
static bool s_game_over;

static void trigger_game_over(void);
static void trigger_win(void);

static lv_obj_t *new_seg_obj(void)
{
    lv_obj_t *o = lv_obj_create(s_scr);
    if (!o) return NULL; // LVGL 对象池耗尽时不崩溃
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, CELL - 1, CELL - 1);
    lv_obj_set_style_radius(o, 4, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

static void update_hud(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "得分: %d", s_score);
    if (s_score_label) lv_label_set_text(s_score_label, buf);
    if (s_score > s_high_score) s_high_score = s_score;
    snprintf(buf, sizeof(buf), "最高: %d", s_high_score);
    if (s_high_label) lv_label_set_text(s_high_label, buf);
}

// 蛇头圆润小眼睛（自然圆黑眼珠 + 白色高光反光微粒）
static void update_head_decorations(lv_obj_t *head_obj, int dir)
{
    if (!head_obj) return;

    if (!s_head_eye1) {
        s_head_eye1 = lv_obj_create(head_obj);
        lv_obj_remove_flag(s_head_eye1, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_head_eye1, 3, 3);
        lv_obj_set_style_radius(s_head_eye1, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_head_eye1, lv_color_hex(0x151B26), 0);
        lv_obj_set_style_border_width(s_head_eye1, 0, 0);
        lv_obj_set_style_pad_all(s_head_eye1, 0, 0);

        s_head_glint1 = lv_obj_create(s_head_eye1);
        lv_obj_remove_flag(s_head_glint1, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_head_glint1, 0, 0);
        lv_obj_set_size(s_head_glint1, 1, 1);
        lv_obj_set_style_bg_color(s_head_glint1, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(s_head_glint1, 0, 0);
        lv_obj_set_style_pad_all(s_head_glint1, 0, 0);
    }
    if (!s_head_eye2) {
        s_head_eye2 = lv_obj_create(head_obj);
        lv_obj_remove_flag(s_head_eye2, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_head_eye2, 3, 3);
        lv_obj_set_style_radius(s_head_eye2, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_head_eye2, lv_color_hex(0x151B26), 0);
        lv_obj_set_style_border_width(s_head_eye2, 0, 0);
        lv_obj_set_style_pad_all(s_head_eye2, 0, 0);

        s_head_glint2 = lv_obj_create(s_head_eye2);
        lv_obj_remove_flag(s_head_glint2, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_head_glint2, 0, 0);
        lv_obj_set_size(s_head_glint2, 1, 1);
        lv_obj_set_style_bg_color(s_head_glint2, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(s_head_glint2, 0, 0);
        lv_obj_set_style_pad_all(s_head_glint2, 0, 0);
    }

    if (lv_obj_get_parent(s_head_eye1) != head_obj) lv_obj_set_parent(s_head_eye1, head_obj);
    if (lv_obj_get_parent(s_head_eye2) != head_obj) lv_obj_set_parent(s_head_eye2, head_obj);

    // 根据朝向 (0上 1右 2下 3左) 布局圆润小眼睛
    switch (dir) {
        case 0: // 向上
            lv_obj_set_pos(s_head_eye1, 2, 2);
            lv_obj_set_pos(s_head_eye2, 6, 2);
            break;
        case 1: // 向右
            lv_obj_set_pos(s_head_eye1, 6, 2);
            lv_obj_set_pos(s_head_eye2, 6, 6);
            break;
        case 2: // 向下
            lv_obj_set_pos(s_head_eye1, 2, 6);
            lv_obj_set_pos(s_head_eye2, 6, 6);
            break;
        case 3: // 向左
            lv_obj_set_pos(s_head_eye1, 2, 2);
            lv_obj_set_pos(s_head_eye2, 2, 6);
            break;
    }
}

static void sync_visuals(void)
{
    for (int i = 0; i < s_len; i++) {
        lv_obj_t *o = s_seg_objs[i];
        if (!o) continue;
        lv_obj_set_pos(o, BOARD_LEFT + s_snake[i].x * CELL, BOARD_TOP + s_snake[i].y * CELL);

        if (i == 0) {
            // 蛇头: 全圆润胶囊形态 + 荧光翠绿 + 细腻暗绿边框
            lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(o, lv_color_hex(0x2ECC71), 0);
            lv_obj_set_style_border_color(o, lv_color_hex(0x196F3D), 0);
            lv_obj_set_style_border_width(o, 1, 0);
            update_head_decorations(o, s_dir);
        } else {
            // 蛇身: 柔和圆角 + 经典双色翡翠绿阶梯
            lv_obj_set_style_radius(o, 3, 0);
            lv_obj_set_style_border_width(o, 0, 0);
            if (i % 2 == 1) {
                lv_obj_set_style_bg_color(o, lv_color_hex(0x27AE60), 0);
            } else {
                lv_obj_set_style_bg_color(o, lv_color_hex(0x229954), 0);
            }
        }
    }
}

static void apply_food_pos(void)
{
    if (!s_food_obj) return;
    lv_obj_set_pos(s_food_obj, BOARD_LEFT + s_food.x * CELL, BOARD_TOP + s_food.y * CELL);
    lv_obj_remove_flag(s_food_obj, LV_OBJ_FLAG_HIDDEN);
}

static void place_food(void)
{
    for (int tries = 0; tries < 200; tries++) {
        int x = rand() % COLS;
        int y = rand() % ROWS;
        bool on_snake = false;
        for (int i = 0; i < s_len; i++) {
            if (s_snake[i].x == x && s_snake[i].y == y) {
                on_snake = true;
                break;
            }
        }
        if (!on_snake) {
            s_food.x = (int16_t)x;
            s_food.y = (int16_t)y;
            apply_food_pos();
            return;
        }
    }
    // 线性兜底
    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            bool on_snake = false;
            for (int i = 0; i < s_len; i++) {
                if (s_snake[i].x == x && s_snake[i].y == y) {
                    on_snake = true;
                    break;
                }
            }
            if (!on_snake) {
                s_food.x = (int16_t)x;
                s_food.y = (int16_t)y;
                apply_food_pos();
                return;
            }
        }
    }
    trigger_win();
}

static void show_panel(const char *title, bool win)
{
    s_game_over = true;
    if (s_score > s_high_score) s_high_score = s_score;
    game_audio_play(win ? GAME_SFX_WIN : GAME_SFX_OVER);

    if (!s_game_over_panel) {
        s_game_over_panel = ui_system_item_create(s_scr, 24, 90, 192, 140);
        lv_obj_set_style_bg_color(s_game_over_panel, lv_color_hex(0x2C3E50), 0);
        lv_obj_set_style_border_color(s_game_over_panel, lv_color_hex(0xE74C3C), 0);
        lv_obj_set_style_border_width(s_game_over_panel, 3, 0);

        s_over_title_label = ui_system_label(s_game_over_panel, title, &ui_font_noto_sc_14, 0xFFFFFF);
        lv_obj_set_width(s_over_title_label, 192);
        lv_obj_set_style_text_align(s_over_title_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_over_title_label, 0, 16);

        s_over_score_label = ui_system_label(s_game_over_panel, "", &ui_font_noto_sc_14, 0xF1C40F);
        lv_obj_set_width(s_over_score_label, 192);
        lv_obj_set_style_text_align(s_over_score_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_over_score_label, 0, 50);

        lv_obj_t *hint = ui_system_label(s_game_over_panel, "按 OK 重玩  长按返回", &ui_font_noto_sc_14, 0xBDC3C7);
        lv_obj_set_width(hint, 192);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(hint, 0, 95);
    }

    if (s_over_title_label) lv_label_set_text(s_over_title_label, title);
    char buf[48];
    snprintf(buf, sizeof(buf), "本次得分: %d\n最高得分: %d", s_score, s_high_score);
    lv_label_set_text(s_over_score_label, buf);
    lv_obj_remove_flag(s_game_over_panel, LV_OBJ_FLAG_HIDDEN);
}

static void trigger_game_over(void)
{
    show_panel("游戏结束", false);
}

static void trigger_win(void)
{
    show_panel("大获全胜!", true);
}

static void snake_clear_segments(void)
{
    s_head_eye1 = NULL;
    s_head_glint1 = NULL;
    s_head_eye2 = NULL;
    s_head_glint2 = NULL;
    for (int i = 0; i < s_len; i++) {
        if (s_seg_objs[i]) {
            lv_obj_delete(s_seg_objs[i]);
            s_seg_objs[i] = NULL;
        }
    }
}

static void snake_reset(void)
{
    snake_clear_segments();
    s_len = 3;
    s_snake[0].x = 5; s_snake[0].y = 10;
    s_snake[1].x = 4; s_snake[1].y = 10;
    s_snake[2].x = 3; s_snake[2].y = 10;
    for (int i = 0; i < s_len; i++) {
        s_seg_objs[i] = new_seg_obj();
    }
    s_dir = 1; // 初始向右
    s_turn_queue_len = 0;
    s_score = 0;
    s_move_accum = 0;
    s_paused = false;
    s_game_over = false;
    if (s_game_over_panel) lv_obj_add_flag(s_game_over_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_pause_label) lv_obj_add_flag(s_pause_label, LV_OBJ_FLAG_HIDDEN);
    update_hud();
    sync_visuals();
    place_food();
}

static void step_snake(void)
{
    if (s_turn_queue_len > 0) {
        s_dir = s_turn_queue[0];
        s_turn_queue[0] = s_turn_queue[1];
        s_turn_queue_len--;
    }

    int nx = s_snake[0].x + DX[s_dir];
    int ny = s_snake[0].y + DY[s_dir];
    if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS) {
        trigger_game_over();
        return;
    }

    bool eating = (nx == s_food.x && ny == s_food.y);
    int check_len = eating ? s_len : s_len - 1;
    for (int i = 0; i < check_len; i++) {
        if (s_snake[i].x == nx && s_snake[i].y == ny) {
            trigger_game_over();
            return;
        }
    }

    if (eating) {
        lv_obj_t *head_obj = new_seg_obj();
        if (!head_obj) {
            trigger_game_over();
            return;
        }
        for (int i = s_len; i > 0; i--) {
            s_snake[i] = s_snake[i - 1];
            s_seg_objs[i] = s_seg_objs[i - 1];
        }
        s_len++;
        s_seg_objs[0] = head_obj;
        s_score += 10;
        update_hud();
        game_audio_play(GAME_SFX_POP);
        if (s_len >= COLS * ROWS) {
            trigger_win();
            return;
        }
        place_food();
    } else {
        lv_obj_t *tail_obj = s_seg_objs[s_len - 1];
        for (int i = s_len - 1; i > 0; i--) {
            s_snake[i] = s_snake[i - 1];
            s_seg_objs[i] = s_seg_objs[i - 1];
        }
        s_seg_objs[0] = tail_obj;
    }

    s_snake[0].x = (int16_t)nx;
    s_snake[0].y = (int16_t)ny;
    sync_visuals();
}

static void snake_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_game_over || s_paused) return;

    s_move_accum += TICK_PERIOD_MS;
    int period = 210 - (s_len - 3) * 4;
    if (period < 80) period = 80;
    if (s_move_accum < period) return;
    s_move_accum -= period;
    step_snake();
}

static void queue_turn(int d)
{
    if (s_turn_queue_len >= 2) return;
    int base_dir = (s_turn_queue_len > 0) ? s_turn_queue[s_turn_queue_len - 1] : s_dir;
    int nd = (base_dir + d + 4) % 4;
    if (s_len > 1 && (nd + 2) % 4 == base_dir) return; // 禁止 180 度掉头
    s_turn_queue[s_turn_queue_len++] = nd;
    game_audio_play(GAME_SFX_MOVE);
}

void demo_game_snake_enter(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x10131A), 0);

    // 棋盘背景板 (216px 宽 x 252px 高, 居中呈现)
    lv_obj_t *board_bg = lv_obj_create(s_scr);
    if (board_bg) {
        lv_obj_remove_flag(board_bg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(board_bg, BOARD_LEFT - 3, BOARD_TOP - 3);
        lv_obj_set_size(board_bg, COLS * CELL + 6, ROWS * CELL + 6);
        lv_obj_set_style_radius(board_bg, 8, 0);
        lv_obj_set_style_bg_color(board_bg, lv_color_hex(0x1B2028), 0);
        lv_obj_set_style_bg_opa(board_bg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(board_bg, lv_color_hex(0x3A4150), 0);
        lv_obj_set_style_border_width(board_bg, 2, 0);
        lv_obj_set_style_pad_all(board_bg, 0, 0);
    }

    s_score_label = ui_system_label(s_scr, "得分: 0", &ui_font_noto_sc_14, 0xF3F1EB);
    lv_obj_set_pos(s_score_label, 12, 8);
    s_high_label = ui_system_label(s_scr, "最高: 0", &ui_font_noto_sc_14, 0xA6ABB5);
    lv_obj_set_pos(s_high_label, 120, 8);

    lv_obj_t *hint = ui_system_label(s_scr, "上/下:转向  OK:暂停  长按:返回", &ui_font_noto_sc_14, 0x7F8C8D);
    lv_obj_set_pos(hint, 12, 294);

    s_pause_label = ui_system_label(s_scr, "已暂停", &ui_font_noto_sc_14, 0xFFD700);
    lv_obj_set_width(s_pause_label, 80);
    lv_obj_set_style_text_align(s_pause_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_pause_label, 80, 140);
    lv_obj_add_flag(s_pause_label, LV_OBJ_FLAG_HIDDEN);

    // 食物: 红苹果主体 + 白色高光 + 绿叶
    s_food_obj = lv_obj_create(s_scr);
    if (s_food_obj) {
        lv_obj_remove_flag(s_food_obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_food_obj, CELL - 1, CELL - 1);
        lv_obj_set_style_radius(s_food_obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_food_obj, lv_color_hex(0xE74C3C), 0);
        lv_obj_set_style_border_width(s_food_obj, 0, 0);
        lv_obj_set_style_pad_all(s_food_obj, 0, 0);

        s_food_shine = lv_obj_create(s_food_obj);
        if (s_food_shine) {
            lv_obj_remove_flag(s_food_shine, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(s_food_shine, 2, 2);
            lv_obj_set_size(s_food_shine, 2, 2);
            lv_obj_set_style_radius(s_food_shine, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(s_food_shine, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(s_food_shine, LV_OPA_70, 0);
            lv_obj_set_style_border_width(s_food_shine, 0, 0);
            lv_obj_set_style_pad_all(s_food_shine, 0, 0);
        }

        s_food_leaf = lv_obj_create(s_food_obj);
        if (s_food_leaf) {
            lv_obj_remove_flag(s_food_leaf, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(s_food_leaf, 4, 1);
            lv_obj_set_size(s_food_leaf, 3, 2);
            lv_obj_set_style_radius(s_food_leaf, 1, 0);
            lv_obj_set_style_bg_color(s_food_leaf, lv_color_hex(0x2ECC71), 0);
            lv_obj_set_style_border_width(s_food_leaf, 0, 0);
            lv_obj_set_style_pad_all(s_food_leaf, 0, 0);
        }
    }

    s_game_over_panel = NULL;
    s_over_title_label = NULL;
    s_over_score_label = NULL;
    s_head_eye1 = NULL;
    s_head_glint1 = NULL;
    s_head_eye2 = NULL;
    s_head_glint2 = NULL;
    for (int i = 0; i < MAX_SEG; i++) s_seg_objs[i] = NULL;

    snake_reset();
    s_timer = lv_timer_create(snake_tick_cb, TICK_PERIOD_MS, NULL);
    lv_screen_load(s_scr);
}

void demo_game_snake_exit(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    for (int i = 0; i < MAX_SEG; i++) s_seg_objs[i] = NULL;
    s_len = 0;
    s_score_label = NULL;
    s_high_label = NULL;
    s_pause_label = NULL;
    s_food_obj = NULL;
    s_food_shine = NULL;
    s_food_leaf = NULL;
    s_head_eye1 = NULL;
    s_head_glint1 = NULL;
    s_head_eye2 = NULL;
    s_head_glint2 = NULL;
    s_game_over_panel = NULL;
    s_over_title_label = NULL;
    s_over_score_label = NULL;
}

void demo_game_snake_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (s_game_over) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            game_audio_play(GAME_SFX_POP);
            snake_reset();
        }
        return;
    }

    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) {
            s_paused = !s_paused;
            game_audio_play(GAME_SFX_MOVE);
            if (s_pause_label) {
                if (s_paused) {
                    lv_obj_remove_flag(s_pause_label, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_pause_label, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        return;
    }

    if (s_paused) return;

    if (ev == BSP_BTN_PRESS) {
        if (btn == BSP_BTN_UP) queue_turn(-1);
        else if (btn == BSP_BTN_DOWN) queue_turn(1);
    }
}
