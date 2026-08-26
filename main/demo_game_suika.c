// main/demo_game_suika.c - 《合成大西瓜》(Suika Game)
#include "demo_game_suika.h"
#include "game_audio.h"
#include "ui_system.h"
#include "ui_status.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "bsp_button.h"
#include "lvgl.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>


#define MAX_FRUITS          32
#define MAX_PARTICLES       16
#define CONTAINER_LEFT      12
#define CONTAINER_RIGHT     228
#define CONTAINER_BOTTOM    308
#define DANGER_LINE_Y       64
#define DROP_Y              44
#define TICK_PERIOD_MS      20

typedef struct {
    uint8_t radius;
    uint32_t color;
    uint32_t border_color;
    uint16_t score;
} fruit_def_t;

static const fruit_def_t FRUIT_TIERS[] = {
    { 9,  0x9C27B0, 0x7B1FA2, 2   }, // 0: 葡萄 Grape (Purple)
    { 12, 0xE91E63, 0xC2185B, 4   }, // 1: 草莓 Strawberry (Crimson)
    { 15, 0xFF9800, 0xE65100, 8   }, // 2: 橘子 Orange (Tangerine)
    { 18, 0xFFEB3B, 0xF57F17, 16  }, // 3: 柠檬 Lemon (Yellow)
    { 22, 0x8BC34A, 0x5D4037, 32  }, // 4: 猕猴桃 Kiwi (Lime Green + Brown rim)
    { 26, 0xFF5722, 0xBF360C, 64  }, // 5: 柿子/西红柿 Tomato (Coral)
    { 30, 0xF48FB1, 0xC2185B, 128 }, // 6: 桃子 Peach (Pastel Pink)
    { 35, 0xFFB300, 0xE65100, 256 }, // 7: 菠萝 Pineapple (Golden)
    { 40, 0x6D4C41, 0x3E2723, 512 }, // 8: 椰子 Coconut (Cocoa Brown)
    { 46, 0x2ECC71, 0x1B5E20, 1000}, // 9: 超大西瓜 Watermelon (Emerald Green)
};
#define FRUIT_TIER_COUNT (sizeof(FRUIT_TIERS) / sizeof(FRUIT_TIERS[0]))

typedef struct {
    bool active;
    bool sleeping;
    uint8_t tier;
    uint8_t sleep_ticks;
    float x, y;
    float vx, vy;
    int16_t last_px, last_py;
    lv_obj_t *obj;
} fruit_t;

typedef struct {
    bool active;
    float x, y;
    float vx, vy;
    int life;
    lv_obj_t *obj;
} particle_t;

// 游戏状态
static lv_obj_t *s_scr;
static lv_obj_t *s_score_label;
static lv_obj_t *s_high_score_label;
static lv_obj_t *s_next_preview_obj;
static lv_obj_t *s_dropper_obj;
static lv_obj_t *s_danger_line;
static lv_obj_t *s_game_over_panel;
static lv_obj_t *s_over_score_label;
static lv_obj_t *s_float_score_label;
static lv_timer_t *s_timer;

static fruit_t s_fruits[MAX_FRUITS];
static particle_t s_particles[MAX_PARTICLES];
static uint32_t s_score = 0;
static uint32_t s_high_score = 0;
static uint8_t s_current_tier = 0;
static uint8_t s_next_tier = 0;
static float s_dropper_x = 120.0f;
static bool s_is_dropping = false;
static int s_dropping_slot = -1;
static int s_drop_cooldown = 0;
static bool s_game_over = false;
static int s_danger_counter = 0;
static int s_float_score_life = 0;

static void create_fruit_visual(fruit_t *f);
static void destroy_fruit_visual(fruit_t *f);
static void spawn_merge_particles(float x, float y, uint32_t color);

static lv_obj_t *add_circle(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *add_rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius, lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static void build_fruit_pattern(lv_obj_t *parent, uint8_t tier, int r)
{
    // Specular glossy reflection
    int shine_s = (r >= 24) ? 6 : ((r >= 14) ? 4 : 2);
    add_circle(parent, r / 3, r / 4, shine_s, shine_s, 0xFFFFFF, LV_OPA_80);

    switch (tier) {
    case 0: // 🍇 Grape
        add_rect(parent, r - 2, 0, 4, 3, 0x4CAF50, 2, LV_OPA_COVER);
        break;

    case 1: // 🍓 Strawberry
        add_rect(parent, r - 4, 0, 8, 3, 0x4CAF50, 2, LV_OPA_COVER);
        add_circle(parent, r - 3, r - 1, 2, 2, 0xFFEB3B, LV_OPA_COVER);
        add_circle(parent, r + 1, r + 1, 2, 2, 0xFFEB3B, LV_OPA_COVER);
        break;

    case 2: // 🍊 Orange
        add_rect(parent, r - 2, 0, 4, 3, 0x4CAF50, 2, LV_OPA_COVER);
        add_circle(parent, r - 2, r - 2, 4, 4, 0xFFE082, LV_OPA_COVER);
        break;

    case 3: // 🍋 Lemon
        add_rect(parent, r - 2, 0, 5, 3, 0x4CAF50, 2, LV_OPA_COVER);
        add_circle(parent, r - 3, r - 3, 6, 6, 0xFFFFFF, LV_OPA_70);
        break;

    case 4: // 🥝 Kiwi
        add_circle(parent, r - 4, r - 4, 8, 8, 0xDCEDC8, LV_OPA_COVER);
        add_circle(parent, r - 6, r - 2, 2, 2, 0x212121, LV_OPA_COVER);
        add_circle(parent, r + 4, r - 2, 2, 2, 0x212121, LV_OPA_COVER);
        add_circle(parent, r - 1, r + 4, 2, 2, 0x212121, LV_OPA_COVER);
        break;

    case 5: // 🍅 Tomato
        add_rect(parent, r - 5, 0, 10, 3, 0x2E7D32, 2, LV_OPA_COVER);
        add_circle(parent, r - 6, r - 2, 3, 3, 0x1A252C, LV_OPA_COVER);
        add_circle(parent, r + 3, r - 2, 3, 3, 0x1A252C, LV_OPA_COVER);
        add_circle(parent, r - 9, r + 2, 4, 3, 0xFF8A80, LV_OPA_70);
        add_circle(parent, r + 5, r + 2, 4, 3, 0xFF8A80, LV_OPA_70);
        break;

    case 6: // 🍑 Peach
        add_rect(parent, r - 1, 3, 2, 10, 0xAD1457, 1, LV_OPA_40);
        add_rect(parent, r - 5, 0, 6, 4, 0x4CAF50, 2, LV_OPA_COVER);
        add_circle(parent, r - 11, r + 2, 6, 4, 0xFF4081, LV_OPA_60);
        add_circle(parent, r + 5, r + 2, 6, 4, 0xFF4081, LV_OPA_60);
        break;

    case 7: // 🍍 Pineapple
        add_rect(parent, r - 6, 0, 12, 7, 0x388E3C, 2, LV_OPA_COVER);
        add_rect(parent, r - 10, r - 6, 5, 5, 0xF57C00, 2, LV_OPA_40);
        add_rect(parent, r + 5, r - 6, 5, 5, 0xF57C00, 2, LV_OPA_40);
        add_circle(parent, r - 6, r - 1, 3, 4, 0x212121, LV_OPA_COVER);
        add_circle(parent, r + 3, r - 1, 3, 4, 0x212121, LV_OPA_COVER);
        break;

    case 8: // 🥥 Coconut
        add_circle(parent, 3, 3, r * 2 - 6, r * 2 - 6, 0xF5F5F5, LV_OPA_30);
        add_circle(parent, r - 8, r - 8, 4, 4, 0x3E2723, LV_OPA_COVER);
        add_circle(parent, r + 4, r - 8, 4, 4, 0x3E2723, LV_OPA_COVER);
        add_circle(parent, r - 2, r - 10, 4, 4, 0x3E2723, LV_OPA_COVER);
        break;

    case 9: // 🍉 Giant Watermelon
        add_rect(parent, r - 18, 4, 4, r * 2 - 8, 0x196F3D, 2, LV_OPA_COVER);
        add_rect(parent, r - 3, 2, 4, r * 2 - 4, 0x196F3D, 2, LV_OPA_COVER);
        add_rect(parent, r + 12, 4, 4, r * 2 - 8, 0x196F3D, 2, LV_OPA_COVER);
        add_circle(parent, r - 12, r - 4, 5, 7, 0x0E2F1B, LV_OPA_COVER);
        add_circle(parent, r + 7, r - 4, 5, 7, 0x0E2F1B, LV_OPA_COVER);
        add_circle(parent, r - 16, r + 5, 7, 5, 0xFF5252, LV_OPA_80);
        add_circle(parent, r + 9, r + 5, 7, 5, 0xFF5252, LV_OPA_80);
        add_rect(parent, r - 4, r + 7, 8, 3, 0x0E2F1B, 2, LV_OPA_COVER);
        break;
    }
}

static int8_t s_dropper_built_tier = -1;
static int8_t s_next_built_tier = -1;

static void update_score_display(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "得分: %lu", (unsigned long)s_score);
    if (s_score_label) lv_label_set_text(s_score_label, buf);

    if (s_score > s_high_score) {
        s_high_score = s_score;
    }
    snprintf(buf, sizeof(buf), "最高: %lu", (unsigned long)s_high_score);
    if (s_high_score_label) lv_label_set_text(s_high_score_label, buf);
}

static void update_dropper_visual(void)
{
    if (!s_dropper_obj || s_game_over) return;

    if (s_is_dropping) {
        lv_obj_add_flag(s_dropper_obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(s_dropper_obj, LV_OBJ_FLAG_HIDDEN);
    const fruit_def_t *def = &FRUIT_TIERS[s_current_tier];
    int r = def->radius;
    int d = r * 2;

    if (s_dropper_built_tier != s_current_tier) {
        s_dropper_built_tier = s_current_tier;
        lv_obj_set_size(s_dropper_obj, d, d);
        lv_obj_set_style_bg_color(s_dropper_obj, lv_color_hex(def->color), 0);
        lv_obj_set_style_border_color(s_dropper_obj, lv_color_hex(def->border_color), 0);
        lv_obj_set_style_border_width(s_dropper_obj, (r >= 22) ? 3 : 2, 0);
        lv_obj_clean(s_dropper_obj);
        build_fruit_pattern(s_dropper_obj, s_current_tier, r);
    }

    lv_obj_set_pos(s_dropper_obj, (int)(s_dropper_x - r), (int)(DROP_Y - r));

    // Update next preview only when next tier changes
    if (s_next_preview_obj && s_next_built_tier != s_next_tier) {
        s_next_built_tier = s_next_tier;
        const fruit_def_t *next_def = &FRUIT_TIERS[s_next_tier];
        lv_obj_set_style_bg_color(s_next_preview_obj, lv_color_hex(next_def->color), 0);
        lv_obj_set_style_border_color(s_next_preview_obj, lv_color_hex(next_def->border_color), 0);
        lv_obj_clean(s_next_preview_obj);
        build_fruit_pattern(s_next_preview_obj, s_next_tier, 10);
    }
}

static void create_fruit_visual(fruit_t *f)
{
    if (!s_scr || f->obj) return;

    const fruit_def_t *def = &FRUIT_TIERS[f->tier];
    int r = def->radius;
    int d = r * 2;

    f->obj = lv_obj_create(s_scr);
    lv_obj_remove_flag(f->obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(f->obj, d, d);
    f->last_px = (int16_t)roundf(f->x - r);
    f->last_py = (int16_t)roundf(f->y - r);
    lv_obj_set_pos(f->obj, f->last_px, f->last_py);
    lv_obj_set_style_radius(f->obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(f->obj, lv_color_hex(def->color), 0);
    lv_obj_set_style_border_color(f->obj, lv_color_hex(def->border_color), 0);
    lv_obj_set_style_border_width(f->obj, (r >= 22) ? 3 : 2, 0);
    lv_obj_set_style_pad_all(f->obj, 0, 0);

    build_fruit_pattern(f->obj, f->tier, r);
}

static void destroy_fruit_visual(fruit_t *f)
{
    if (f->obj) {
        lv_obj_delete(f->obj);
        f->obj = NULL;
    }
}

static void show_float_score(float x, float y, uint16_t score)
{
    if (!s_float_score_label) {
        s_float_score_label = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_float_score_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_float_score_label, lv_color_hex(0xFFEB3B), 0);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "+%u", score);
    lv_label_set_text(s_float_score_label, buf);
    lv_obj_set_pos(s_float_score_label, (int)x - 10, (int)y - 12);
    lv_obj_remove_flag(s_float_score_label, LV_OBJ_FLAG_HIDDEN);
    s_float_score_life = 20; // 20 frames
}

static void spawn_merge_particles(float x, float y, uint32_t color)
{
    for (int k = 0; k < 6; k++) {
        for (int p = 0; p < MAX_PARTICLES; p++) {
            if (!s_particles[p].active) {
                s_particles[p].active = true;
                s_particles[p].x = x;
                s_particles[p].y = y;
                float angle = ((float)(rand() % 360)) * (3.14159f / 180.0f);
                float speed = 1.2f + ((float)(rand() % 100)) / 45.0f;
                s_particles[p].vx = cosf(angle) * speed;
                s_particles[p].vy = sinf(angle) * speed - 1.0f;
                s_particles[p].life = 12 + (rand() % 8);

                if (!s_particles[p].obj) {
                    s_particles[p].obj = lv_obj_create(s_scr);
                    lv_obj_remove_flag(s_particles[p].obj, LV_OBJ_FLAG_SCROLLABLE);
                    lv_obj_set_size(s_particles[p].obj, 4, 4);
                    lv_obj_set_style_radius(s_particles[p].obj, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_border_width(s_particles[p].obj, 0, 0);
                }
                lv_obj_set_style_bg_color(s_particles[p].obj, lv_color_hex(color), 0);
                lv_obj_remove_flag(s_particles[p].obj, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(s_particles[p].obj, (int)x, (int)y);
                break;
            }
        }
    }
}

static void trigger_game_over(void)
{
    s_game_over = true;
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
    snprintf(buf, sizeof(buf), "本次得分: %lu\n最高得分: %lu", (unsigned long)s_score, (unsigned long)s_high_score);
    lv_label_set_text(s_over_score_label, buf);
    lv_obj_remove_flag(s_game_over_panel, LV_OBJ_FLAG_HIDDEN);
}

static void suika_reset_game(void)
{
    s_score = 0;
    s_game_over = false;
    s_is_dropping = false;
    s_dropping_slot = -1;
    s_drop_cooldown = 0;
    s_danger_counter = 0;
    s_current_tier = rand() % 3; // Start with tier 0..2
    s_next_tier = rand() % 4;
    s_dropper_built_tier = -1;
    s_next_built_tier = -1;
    s_dropper_x = 120.0f;

    for (int i = 0; i < MAX_FRUITS; i++) {
        if (s_fruits[i].active) {
            destroy_fruit_visual(&s_fruits[i]);
            s_fruits[i].active = false;
        }
        s_fruits[i].sleeping = false;
        s_fruits[i].sleep_ticks = 0;
    }

    for (int p = 0; p < MAX_PARTICLES; p++) {
        if (s_particles[p].obj) {
            lv_obj_add_flag(s_particles[p].obj, LV_OBJ_FLAG_HIDDEN);
        }
        s_particles[p].active = false;
    }

    if (s_game_over_panel) {
        lv_obj_add_flag(s_game_over_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_float_score_label) {
        lv_obj_add_flag(s_float_score_label, LV_OBJ_FLAG_HIDDEN);
    }

    update_score_display();
    update_dropper_visual();
}

static void suika_physics_step(void)
{
    if (s_game_over) return;

    // 0. Handle drop cooldown & strictly enforce single dropping fruit
    if (s_is_dropping) {
        if (s_drop_cooldown > 0) {
            s_drop_cooldown--;
        }
        bool ready_for_next = (s_drop_cooldown == 0);
        if (s_dropping_slot >= 0 && s_fruits[s_dropping_slot].active) {
            fruit_t *df = &s_fruits[s_dropping_slot];
            // Do not release until the dropped fruit has moved down or settled
            if (df->y < (DANGER_LINE_Y + 12) && fabsf(df->vy) > 0.4f && s_drop_cooldown > 8) {
                ready_for_next = false;
            }
        }
        if (ready_for_next) {
            s_is_dropping = false;
            s_dropping_slot = -1;
            s_current_tier = s_next_tier;
            s_next_tier = rand() % 4; // next is tier 0..3
            update_dropper_visual();
        }
    }

    // 1. Update fruit kinematics & wall collisions (skip sleeping fruits)
    for (int i = 0; i < MAX_FRUITS; i++) {
        if (!s_fruits[i].active) continue;

        fruit_t *f = &s_fruits[i];
        if (f->sleeping) continue;

        int r = FRUIT_TIERS[f->tier].radius;

        f->vy += 0.38f; // Gravity
        f->vx *= 0.980f; // Air damping
        f->vy *= 0.980f;

        f->x += f->vx;
        f->y += f->vy;

        // Bottom floor
        if (f->y + r > CONTAINER_BOTTOM) {
            f->y = (float)(CONTAINER_BOTTOM - r);
            f->vy = -f->vy * 0.15f;
            f->vx *= 0.70f;
            if (fabsf(f->vy) < 0.18f) f->vy = 0.0f;
            if (fabsf(f->vx) < 0.05f) f->vx = 0.0f;
        }

        // Left wall
        if (f->x - r < CONTAINER_LEFT) {
            f->x = (float)(CONTAINER_LEFT + r);
            f->vx = -f->vx * 0.20f;
            if (fabsf(f->vx) < 0.05f) f->vx = 0.0f;
        }

        // Right wall
        if (f->x + r > CONTAINER_RIGHT) {
            f->x = (float)(CONTAINER_RIGHT - r);
            f->vx = -f->vx * 0.20f;
            if (fabsf(f->vx) < 0.05f) f->vx = 0.0f;
        }

        // Physics Sleep Check (Resting fruits freeze to avoid CPU waste and jitter)
        if (fabsf(f->vx) < 0.05f && fabsf(f->vy) < 0.08f) {
            f->sleep_ticks++;
            if (f->sleep_ticks > 8) {
                f->sleeping = true;
                f->vx = 0.0f;
                f->vy = 0.0f;
            }
        } else {
            f->sleep_ticks = 0;
        }
    }

    // 2. Fruit-Fruit collision and Merge detection (multi-body solver)
    for (int i = 0; i < MAX_FRUITS; i++) {
        if (!s_fruits[i].active) continue;

        for (int j = i + 1; j < MAX_FRUITS; j++) {
            if (!s_fruits[j].active) continue;

            fruit_t *fa = &s_fruits[i];
            fruit_t *fb = &s_fruits[j];

            // If BOTH fruits are sleeping, skip completely!
            if (fa->sleeping && fb->sleeping) continue;

            float dx = fb->x - fa->x;
            float dy = fb->y - fa->y;
            float dist = sqrtf(dx * dx + dy * dy);
            float min_dist = (float)(FRUIT_TIERS[fa->tier].radius + FRUIT_TIERS[fb->tier].radius);

            if (dist < min_dist && dist > 0.001f) {
                // Wake up both fruits
                fa->sleeping = false;
                fb->sleeping = false;
                fa->sleep_ticks = 0;
                fb->sleep_ticks = 0;

                // Check Merge
                if (fa->tier == fb->tier && fa->tier < (FRUIT_TIER_COUNT - 1)) {
                    // Merge!
                    float mid_x = (fa->x + fb->x) * 0.5f;
                    float mid_y = (fa->y + fb->y) * 0.5f;
                    uint8_t new_tier = fa->tier + 1;
                    uint16_t add_score = FRUIT_TIERS[new_tier].score;

                    s_score += add_score;
                    update_score_display();
                    show_float_score(mid_x, mid_y, add_score);
                    spawn_merge_particles(mid_x, mid_y, FRUIT_TIERS[new_tier].color);

                    // Audio feedback
                    if (new_tier <= 3) {
                        game_audio_play(GAME_SFX_MERGE_SMALL);
                    } else if (new_tier <= 7) {
                        game_audio_play(GAME_SFX_MERGE_MED);
                    } else {
                        game_audio_play(GAME_SFX_MERGE_BIG);
                    }

                    // Remove fb, upgrade fa
                    destroy_fruit_visual(fb);
                    fb->active = false;
                    fb->sleeping = true;

                    fa->tier = new_tier;
                    fa->x = mid_x;
                    fa->y = mid_y;
                    fa->vx = (fa->vx + fb->vx) * 0.25f;
                    fa->vy = (fa->vy + fb->vy) * 0.25f - 0.4f; // slight upward pop
                    fa->sleeping = false;
                    fa->sleep_ticks = 0;

                    destroy_fruit_visual(fa);
                    create_fruit_visual(fa);
                    continue;
                }

                // Mass-weighted elastic collision separation
                float ma = (float)(FRUIT_TIERS[fa->tier].radius * FRUIT_TIERS[fa->tier].radius);
                float mb = (float)(FRUIT_TIERS[fb->tier].radius * FRUIT_TIERS[fb->tier].radius);
                float total_m = ma + mb;
                float wa = mb / total_m; // heavier object gets smaller displacement
                float wb = ma / total_m;

                float overlap = min_dist - dist;
                float nx = dx / dist;
                float ny = dy / dist;

                fa->x -= nx * overlap * wa;
                fa->y -= ny * overlap * wa;
                fb->x += nx * overlap * wb;
                fb->y += ny * overlap * wb;

                float k = (fa->vx - fb->vx) * nx + (fa->vy - fb->vy) * ny;
                if (k > 0) {
                    float impulse = k * 0.32f;
                    fa->vx -= impulse * nx * (1.8f * wb);
                    fa->vy -= impulse * ny * (1.8f * wb);
                    fb->vx += impulse * nx * (1.8f * wa);
                    fb->vy += impulse * ny * (1.8f * wa);
                }
            }
        }
    }

    // 3. Update Visual Positions (with dirty pixel caching)
    bool danger_active = false;
    for (int i = 0; i < MAX_FRUITS; i++) {
        if (!s_fruits[i].active) continue;

        fruit_t *f = &s_fruits[i];
        int r = FRUIT_TIERS[f->tier].radius;

        if (f->obj) {
            int px = (int)roundf(f->x - r);
            int py = (int)roundf(f->y - r);
            if (px != f->last_px || py != f->last_py) {
                lv_obj_set_pos(f->obj, px, py);
                f->last_px = (int16_t)px;
                f->last_py = (int16_t)py;
            }
        }

        // Danger check: settled fruit above danger line
        if ((f->y - r) < DANGER_LINE_Y && fabsf(f->vy) < 0.5f && fabsf(f->vx) < 0.5f) {
            danger_active = true;
        }
    }

    if (danger_active) {
        s_danger_counter++;
        if (s_danger_counter > 100) { // ~2 seconds above line
            trigger_game_over();
        }
    } else {
        if (s_danger_counter > 0) s_danger_counter--;
    }

    // 4. Update Particle positions
    for (int p = 0; p < MAX_PARTICLES; p++) {
        if (!s_particles[p].active) continue;

        s_particles[p].x += s_particles[p].vx;
        s_particles[p].y += s_particles[p].vy;
        s_particles[p].vy += 0.2f; // particle gravity
        s_particles[p].life--;

        if (s_particles[p].life <= 0) {
            s_particles[p].active = false;
            if (s_particles[p].obj) {
                lv_obj_add_flag(s_particles[p].obj, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (s_particles[p].obj) {
            lv_obj_set_pos(s_particles[p].obj, (int)s_particles[p].x, (int)s_particles[p].y);
        }
    }

    // 5. Update Floating score label
    if (s_float_score_life > 0) {
        s_float_score_life--;
        int cur_y = lv_obj_get_y(s_float_score_label);
        lv_obj_set_y(s_float_score_label, cur_y - 1);
        if (s_float_score_life == 0) {
            lv_obj_add_flag(s_float_score_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void suika_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    suika_physics_step();
}

static void suika_drop_fruit(void)
{
    if (s_is_dropping || s_game_over) return;

    // Find free fruit slot
    int slot = -1;
    for (int i = 0; i < MAX_FRUITS; i++) {
        if (!s_fruits[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return;

    game_audio_play(GAME_SFX_DROP);

    s_fruits[slot].active = true;
    s_fruits[slot].sleeping = false;
    s_fruits[slot].sleep_ticks = 0;
    s_fruits[slot].tier = s_current_tier;
    s_fruits[slot].x = s_dropper_x;
    s_fruits[slot].y = (float)DROP_Y;
    s_fruits[slot].vx = 0;
    s_fruits[slot].vy = 1.2f;

    create_fruit_visual(&s_fruits[slot]);

    s_is_dropping = true;
    s_dropping_slot = slot;
    s_drop_cooldown = 25; // 500ms min lock until next fruit can be aimed/dropped
    update_dropper_visual();
}

void demo_game_suika_enter(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0xFBF8EB), 0); // Warm fruit-plate cream background

    // Fruit container border (Solid OPA_COVER to skip composite blending)
    lv_obj_t *container = lv_obj_create(s_scr);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(container, CONTAINER_LEFT - 2, DANGER_LINE_Y);
    lv_obj_set_size(container, (CONTAINER_RIGHT - CONTAINER_LEFT) + 4, (CONTAINER_BOTTOM - DANGER_LINE_Y) + 4);
    lv_obj_set_style_radius(container, 14, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(container, lv_color_hex(0xE0D9C8), 0);
    lv_obj_set_style_border_width(container, 2, 0);
    lv_obj_set_style_pad_all(container, 0, 0);

    // Danger line (dashed warning line)
    s_danger_line = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_danger_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_danger_line, CONTAINER_LEFT, DANGER_LINE_Y);
    lv_obj_set_size(s_danger_line, CONTAINER_RIGHT - CONTAINER_LEFT, 2);
    lv_obj_set_style_bg_color(s_danger_line, lv_color_hex(0xFF7675), 0);
    lv_obj_set_style_border_width(s_danger_line, 0, 0);

    // Header: Score & High Score
    s_score_label = ui_system_label(s_scr, "得分: 0", &ui_font_noto_sc_14, 0x2D3436);
    lv_obj_set_pos(s_score_label, 14, 10);

    s_high_score_label = ui_system_label(s_scr, "最高: 0", &ui_font_noto_sc_14, 0x636E72);
    lv_obj_set_pos(s_high_score_label, 14, 26);

    // Next fruit preview plate
    lv_obj_t *next_title = ui_system_label(s_scr, "下一个", &ui_font_noto_sc_14, 0x636E72);
    lv_obj_set_pos(next_title, 150, 10);

    s_next_preview_obj = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_next_preview_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_next_preview_obj, 20, 20);
    lv_obj_set_pos(s_next_preview_obj, 198, 8);
    lv_obj_set_style_radius(s_next_preview_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_next_preview_obj, 2, 0);
    lv_obj_set_style_pad_all(s_next_preview_obj, 0, 0);

    // Top Dropper Fruit
    s_dropper_obj = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_dropper_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_dropper_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_dropper_obj, 0, 0);

    s_game_over_panel = NULL;
    s_float_score_label = NULL;

    suika_reset_game();

    s_timer = lv_timer_create(suika_timer_cb, TICK_PERIOD_MS, NULL);
    lv_screen_load(s_scr);
}

void demo_game_suika_exit(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    for (int i = 0; i < MAX_FRUITS; i++) {
        destroy_fruit_visual(&s_fruits[i]);
        s_fruits[i].active = false;
    }
    for (int p = 0; p < MAX_PARTICLES; p++) {
        if (s_particles[p].obj) {
            lv_obj_delete(s_particles[p].obj);
            s_particles[p].obj = NULL;
        }
        s_particles[p].active = false;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_score_label = NULL;
        s_high_score_label = NULL;
        s_next_preview_obj = NULL;
        s_dropper_obj = NULL;
        s_danger_line = NULL;
        s_game_over_panel = NULL;
        s_float_score_label = NULL;
    }
}

void demo_game_suika_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;

    if (s_game_over) {
        if (btn == BSP_BTN_OK) {
            game_audio_play(GAME_SFX_POP);
            suika_reset_game();
        }
        return;
    }

    int r = FRUIT_TIERS[s_current_tier].radius;
    int min_x = CONTAINER_LEFT + r;
    int max_x = CONTAINER_RIGHT - r;

    if (btn == BSP_BTN_UP) {
        // Move Left
        s_dropper_x -= 12.0f;
        if (s_dropper_x < min_x) s_dropper_x = (float)min_x;
        game_audio_play(GAME_SFX_MOVE);
        update_dropper_visual();
    } else if (btn == BSP_BTN_DOWN) {
        // Move Right
        s_dropper_x += 12.0f;
        if (s_dropper_x > max_x) s_dropper_x = (float)max_x;
        game_audio_play(GAME_SFX_MOVE);
        update_dropper_visual();
    } else if (btn == BSP_BTN_OK) {
        suika_drop_fruit();
    }
}
