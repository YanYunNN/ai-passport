// main/demo_game_flappy.c - 《像素小鸟》(Flappy Bird)
// 三键方案: OK 或上键 = 拍翅膀(按下即响应, 低延迟)。长按 OK 由 main 拦截返回。
// 内存约束: 最多 4 组水管(8 个对象)+ 小鸟 + 地面 + HUD, 对象量很小。
#include "demo_game_flappy.h"
#include "game_audio.h"
#include "ui_system.h"
#include "ui_font_noto_sc_14.h"
#include "bsp_button.h"
#include "lvgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIRD_X          64
#define BIRD_W          26
#define BIRD_H          20
#define HIT_R           8.0f    // 像素小鸟碰撞半径
#define READY_Y         150.0f
#define GROUND_Y        280
#define GAP             145     // 缺口宽度
#define PIPE_W          36
#define PIPE_SPACING    150
#define PIPE_SPEED      1.6f    // 横移速度
#define GRAVITY         0.26f   // 重力加速度
#define FLAP_VY         -5.2f   // 拍翅跳跃速度
#define MAX_VY          5.0f    // 最大下落速度
#define MAX_PIPES       4
#define TICK_PERIOD_MS  20

typedef struct {
    bool active;
    float x;
    int top_h;   // 上管高度
    int gap_y;   // 上管底 = 下管顶
    bool scored;
    lv_obj_t *top_obj;
    lv_obj_t *bot_obj;
} pipe_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_score_label;
static lv_obj_t *s_high_label;
static lv_obj_t *s_ready_hint;
static lv_obj_t *s_bird_obj;
static lv_obj_t *s_bird_wing[3];
static lv_obj_t *s_game_over_panel;
static lv_obj_t *s_over_score_label;
static lv_timer_t *s_timer;

static pipe_t s_pipes[MAX_PIPES];
static float s_bird_y;
static float s_bird_vy;
static int s_score;
static int s_high_score;
static int s_bob_phase;
static int s_anim_frame;
static int s_anim_tick;
static bool s_started;
static bool s_game_over;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *b = lv_obj_create(parent);
    if (!b) return NULL;
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    return b;
}

static void create_pixel_bird(lv_obj_t *parent)
{
    s_bird_obj = lv_obj_create(parent);
    if (!s_bird_obj) return;
    lv_obj_remove_flag(s_bird_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_bird_obj, BIRD_W, BIRD_H);
    lv_obj_set_style_bg_opa(s_bird_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bird_obj, 0, 0);
    lv_obj_set_style_pad_all(s_bird_obj, 0, 0);

    // 1. 小鸟身体基础层 (金黄主色 + 腹部阴影 + 头部高光)
    box(s_bird_obj, 3, 2, 18, 14, 0xF1C40F, 5);  // 主体金黄
    box(s_bird_obj, 6, 1, 11, 3, 0xFFF59D, 2);   // 头部浅黄高光
    box(s_bird_obj, 4, 12, 15, 4, 0xD4AC0D, 3);  // 腹部金棕阴影

    // 2. 可爱大眼睛 (大白眼白 + 黑眼珠 + 白色高光点)
    box(s_bird_obj, 14, 2, 8, 8, 0xFFFFFF, 4);   // 眼白
    box(s_bird_obj, 18, 4, 3, 4, 0x1A1A1A, 1);   // 黑瞳孔
    box(s_bird_obj, 19, 3, 1, 2, 0xFFFFFF, 0);   // 眼睛反光亮点

    // 3. 鸟喙 (上下喙鲜橙 + 阴影层)
    box(s_bird_obj, 16, 9, 9, 4, 0xFF7043, 2);   // 上喙亮橙
    box(s_bird_obj, 16, 12, 7, 3, 0xD84315, 1);  // 下喙暗红橙
    box(s_bird_obj, 16, 11, 9, 1, 0xBF360C, 0);  // 嘴缝线

    // 4. 可爱粉红腮红
    box(s_bird_obj, 12, 10, 4, 3, 0xFF5252, 2);

    // 5. 三帧拍翅羽翼 (白色羽毛 + 浅灰阴影)
    // 帧 0: 翅膀上扬 (Wing UP)
    s_bird_wing[0] = lv_obj_create(s_bird_obj);
    if (s_bird_wing[0]) {
        lv_obj_remove_flag(s_bird_wing[0], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_bird_wing[0], 2, 3);
        lv_obj_set_size(s_bird_wing[0], 10, 7);
        lv_obj_set_style_bg_opa(s_bird_wing[0], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_bird_wing[0], 0, 0);
        lv_obj_set_style_pad_all(s_bird_wing[0], 0, 0);
        box(s_bird_wing[0], 0, 0, 9, 5, 0xFFFFFF, 2);
        box(s_bird_wing[0], 0, 4, 7, 2, 0xB0BEC5, 1);
        lv_obj_add_flag(s_bird_wing[0], LV_OBJ_FLAG_HIDDEN);
    }

    // 帧 1: 翅膀平飞 (Wing MID)
    s_bird_wing[1] = lv_obj_create(s_bird_obj);
    if (s_bird_wing[1]) {
        lv_obj_remove_flag(s_bird_wing[1], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_bird_wing[1], 2, 7);
        lv_obj_set_size(s_bird_wing[1], 11, 7);
        lv_obj_set_style_bg_opa(s_bird_wing[1], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_bird_wing[1], 0, 0);
        lv_obj_set_style_pad_all(s_bird_wing[1], 0, 0);
        box(s_bird_wing[1], 0, 0, 10, 5, 0xFFFFFF, 3);
        box(s_bird_wing[1], 0, 4, 8, 2, 0xB0BEC5, 1);
    }

    // 帧 2: 翅膀下切 (Wing DOWN)
    s_bird_wing[2] = lv_obj_create(s_bird_obj);
    if (s_bird_wing[2]) {
        lv_obj_remove_flag(s_bird_wing[2], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_bird_wing[2], 3, 10);
        lv_obj_set_size(s_bird_wing[2], 10, 7);
        lv_obj_set_style_bg_opa(s_bird_wing[2], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_bird_wing[2], 0, 0);
        lv_obj_set_style_pad_all(s_bird_wing[2], 0, 0);
        box(s_bird_wing[2], 0, 0, 9, 5, 0xFFFFFF, 2);
        box(s_bird_wing[2], 0, 4, 7, 2, 0xB0BEC5, 1);
        lv_obj_add_flag(s_bird_wing[2], LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_pipe_obj(int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(s_scr);
    if (!o) return NULL;
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 3, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x2ECC71), 0);
    lv_obj_set_style_border_color(o, lv_color_hex(0x1B5E20), 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

static void destroy_pipe(pipe_t *p)
{
    if (p->top_obj) {
        lv_obj_delete(p->top_obj);
        p->top_obj = NULL;
    }
    if (p->bot_obj) {
        lv_obj_delete(p->bot_obj);
        p->bot_obj = NULL;
    }
    p->active = false;
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

static void update_bird_visuals(void)
{
    if (!s_bird_obj) return;
    lv_obj_set_pos(s_bird_obj, (int)(BIRD_X - BIRD_W / 2), (int)(s_bird_y - BIRD_H / 2));

    // 切换当前拍翅帧
    for (int i = 0; i < 3; i++) {
        if (s_bird_wing[i]) {
            if (i == s_anim_frame) lv_obj_remove_flag(s_bird_wing[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_bird_wing[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void spawn_pipe(float x)
{
    int idx = -1;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!s_pipes[i].active) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return;

    int gc = 110 + rand() % 66; // 缺口中心
    pipe_t *p = &s_pipes[idx];
    p->active = true;
    p->x = x;
    p->top_h = gc - GAP / 2;
    p->gap_y = gc + GAP / 2;
    p->scored = false;
    p->top_obj = make_pipe_obj((int)x, 0, PIPE_W, p->top_h);
    p->bot_obj = make_pipe_obj((int)x, p->gap_y, PIPE_W, GROUND_Y - p->gap_y);
}

static void try_spawn_pipe(void)
{
    float rightmost = -1.0f;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (s_pipes[i].active && s_pipes[i].x > rightmost) rightmost = s_pipes[i].x;
    }
    if (rightmost < 0.0f) {
        spawn_pipe(260.0f);
    } else if (rightmost <= 240.0f - PIPE_SPACING) {
        spawn_pipe(rightmost + PIPE_SPACING);
    }
}

static bool circle_rect_hit(float cx, float cy, float r,
                            float rx, float ry, float rw, float rh)
{
    float nx = cx, ny = cy;
    if (cx < rx) nx = rx;
    else if (cx > rx + rw) nx = rx + rw;
    if (cy < ry) ny = ry;
    else if (cy > ry + rh) ny = ry + rh;
    float dx = cx - nx;
    float dy = cy - ny;
    return dx * dx + dy * dy <= r * r;
}

static bool hit_pipe(const pipe_t *p)
{
    if (circle_rect_hit(BIRD_X, s_bird_y, HIT_R,
                        p->x, 0, PIPE_W, (float)p->top_h)) {
        return true;
    }
    if (circle_rect_hit(BIRD_X, s_bird_y, HIT_R,
                        p->x, (float)p->gap_y, PIPE_W,
                        (float)(GROUND_Y - p->gap_y))) {
        return true;
    }
    return false;
}

static void trigger_game_over(void)
{
    s_game_over = true;
    if (s_score > s_high_score) s_high_score = s_score;
    game_audio_play(GAME_SFX_OVER);

    if (!s_game_over_panel) {
        s_game_over_panel = ui_system_item_create(s_scr, 24, 90, 192, 140);
        lv_obj_set_style_bg_color(s_game_over_panel, lv_color_hex(0x2C3E50), 0);
        lv_obj_set_style_border_color(s_game_over_panel, lv_color_hex(0xE74C3C), 0);
        lv_obj_set_style_border_width(s_game_over_panel, 3, 0);

        lv_obj_t *title = ui_system_label(s_game_over_panel, "游戏结束", &ui_font_noto_sc_14, 0xFFFFFF);
        lv_obj_set_width(title, 192);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(title, 0, 16);

        s_over_score_label = ui_system_label(s_game_over_panel, "", &ui_font_noto_sc_14, 0xF1C40F);
        lv_obj_set_width(s_over_score_label, 192);
        lv_obj_set_style_text_align(s_over_score_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_over_score_label, 0, 50);

        lv_obj_t *hint = ui_system_label(s_game_over_panel, "按 OK 重玩  长按返回", &ui_font_noto_sc_14, 0xBDC3C7);
        lv_obj_set_width(hint, 192);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(hint, 0, 95);
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "本次得分: %d\n最高得分: %d", s_score, s_high_score);
    lv_label_set_text(s_over_score_label, buf);
    lv_obj_remove_flag(s_game_over_panel, LV_OBJ_FLAG_HIDDEN);
}

static void flap(void)
{
    s_bird_vy = FLAP_VY;
    s_anim_frame = 2; // 拍翅瞬间翅膀切到下方
    game_audio_play(GAME_SFX_SHOOT);
}

static void flappy_reset(void)
{
    for (int i = 0; i < MAX_PIPES; i++) destroy_pipe(&s_pipes[i]);
    s_bird_y = READY_Y;
    s_bird_vy = 0;
    s_score = 0;
    s_bob_phase = 0;
    s_anim_frame = 1;
    s_anim_tick = 0;
    s_started = false;
    s_game_over = false;
    if (s_game_over_panel) lv_obj_add_flag(s_game_over_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_ready_hint) lv_obj_remove_flag(s_ready_hint, LV_OBJ_FLAG_HIDDEN);
    update_hud();
    update_bird_visuals();
}

static void flappy_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_game_over) return;

    // 拍翅动画轮播 (每 4 ticks ~ 80ms 轮换)
    s_anim_tick++;
    if (s_anim_tick >= 4) {
        s_anim_tick = 0;
        if (!s_started) {
            s_anim_frame = (s_anim_frame + 1) % 3;
        } else {
            if (s_bird_vy < 0) {
                s_anim_frame = (s_anim_frame == 0) ? 2 : 0;
            } else {
                s_anim_frame = 1;
            }
        }
    }

    if (!s_started) {
        // 待机状态: 小鸟上下轻微浮动
        s_bob_phase++;
        s_bird_y = READY_Y + sinf(s_bob_phase * 0.12f) * 6.0f;
        update_bird_visuals();
        return;
    }

    s_bird_vy += GRAVITY;
    if (s_bird_vy > MAX_VY) s_bird_vy = MAX_VY;
    s_bird_y += s_bird_vy;
    if (s_bird_y < HIT_R) {
        s_bird_y = HIT_R;
        s_bird_vy = 0;
    }
    update_bird_visuals();

    for (int i = 0; i < MAX_PIPES; i++) {
        pipe_t *p = &s_pipes[i];
        if (!p->active) continue;
        p->x -= PIPE_SPEED;
        if (p->x < -PIPE_W - 8) {
            destroy_pipe(p);
            continue;
        }
        if (p->top_obj) lv_obj_set_x(p->top_obj, (int)p->x);
        if (p->bot_obj) lv_obj_set_x(p->bot_obj, (int)p->x);

        if (!p->scored && (p->x + PIPE_W) < BIRD_X - HIT_R) {
            p->scored = true;
            s_score++;
            update_hud();
            game_audio_play(GAME_SFX_POP);
        }
        if (hit_pipe(p)) {
            trigger_game_over();
            return;
        }
    }

    if (s_bird_y + HIT_R >= GROUND_Y) {
        trigger_game_over();
        return;
    }

    try_spawn_pipe();
}

void demo_game_flappy_enter(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x4EC0CA), 0); // 经典蓝天底色

    // 远景白云装饰
    for (int i = 0; i < 3; i++) {
        lv_obj_t *cloud = lv_obj_create(s_scr);
        if (cloud) {
            lv_obj_remove_flag(cloud, LV_OBJ_FLAG_SCROLLABLE);
            int cx = 20 + i * 80;
            int cy = 40 + (i % 2) * 35;
            lv_obj_set_pos(cloud, cx, cy);
            lv_obj_set_size(cloud, 54, 18);
            lv_obj_set_style_radius(cloud, 9, 0);
            lv_obj_set_style_bg_color(cloud, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(cloud, LV_OPA_60, 0);
            lv_obj_set_style_border_width(cloud, 0, 0);
            lv_obj_set_style_pad_all(cloud, 0, 0);
        }
    }

    // 地面泥土层与草坪
    lv_obj_t *ground = lv_obj_create(s_scr);
    if (ground) {
        lv_obj_remove_flag(ground, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(ground, 0, GROUND_Y);
        lv_obj_set_size(ground, 240, 320 - GROUND_Y);
        lv_obj_set_style_radius(ground, 0, 0);
        lv_obj_set_style_bg_color(ground, lv_color_hex(0xDED895), 0);
        lv_obj_set_style_border_width(ground, 0, 0);
        lv_obj_set_style_pad_all(ground, 0, 0);
    }
    lv_obj_t *grass = lv_obj_create(s_scr);
    if (grass) {
        lv_obj_remove_flag(grass, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(grass, 0, GROUND_Y);
        lv_obj_set_size(grass, 240, 10);
        lv_obj_set_style_radius(grass, 0, 0);
        lv_obj_set_style_bg_color(grass, lv_color_hex(0x73BF2E), 0);
        lv_obj_set_style_border_color(grass, lv_color_hex(0x558B2F), 0);
        lv_obj_set_style_border_width(grass, 1, 0);
        lv_obj_set_style_pad_all(grass, 0, 0);
    }

    s_score_label = ui_system_label(s_scr, "得分: 0", &ui_font_noto_sc_14, 0xFFFFFF);
    lv_obj_set_pos(s_score_label, 10, 10);
    s_high_label = ui_system_label(s_scr, "最高: 0", &ui_font_noto_sc_14, 0xFFFFFF);
    lv_obj_set_pos(s_high_label, 10, 28);

    s_ready_hint = ui_system_label(s_scr, "按 OK 起飞", &ui_font_noto_sc_14, 0xFFFFFF);
    lv_obj_set_width(s_ready_hint, 120);
    lv_obj_set_style_text_align(s_ready_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ready_hint, 60, 210);

    // 创建像素小鸟实体
    create_pixel_bird(s_scr);

    s_game_over_panel = NULL;
    s_over_score_label = NULL;
    memset(s_pipes, 0, sizeof(s_pipes));

    flappy_reset();
    s_timer = lv_timer_create(flappy_tick_cb, TICK_PERIOD_MS, NULL);
    lv_screen_load(s_scr);
}

void demo_game_flappy_exit(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    for (int i = 0; i < MAX_PIPES; i++) {
        s_pipes[i].top_obj = NULL;
        s_pipes[i].bot_obj = NULL;
        s_pipes[i].active = false;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_score_label = NULL;
    s_high_label = NULL;
    s_ready_hint = NULL;
    s_bird_obj = NULL;
    s_bird_wing[0] = NULL;
    s_bird_wing[1] = NULL;
    s_bird_wing[2] = NULL;
    s_game_over_panel = NULL;
    s_over_score_label = NULL;
}

void demo_game_flappy_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (s_game_over) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            game_audio_play(GAME_SFX_POP);
            flappy_reset();
        }
        return;
    }

    if (ev == BSP_BTN_PRESS && (btn == BSP_BTN_OK || btn == BSP_BTN_UP)) {
        if (!s_started) {
            s_started = true;
            if (s_ready_hint) lv_obj_add_flag(s_ready_hint, LV_OBJ_FLAG_HIDDEN);
        }
        flap();
    }
}
