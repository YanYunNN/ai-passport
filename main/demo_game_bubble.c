// main/demo_game_bubble.c - 《泡泡龙》(Puzzle Bobble)
#include "demo_game_bubble.h"
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


#define GRID_ROWS           11
#define GRID_COLS           8
#define BUBBLE_RADIUS       12
#define BUBBLE_DIAMETER     24
#define ROW_HEIGHT          21
#define BOARD_LEFT          12
#define BOARD_RIGHT         228
#define BOARD_TOP           36
#define CANNON_X            120
#define CANNON_Y            288
#define MAX_PARTICLES       20
#define TICK_PERIOD_MS      20

// 5 种马卡龙糖果泡泡颜色
static const uint32_t BUBBLE_COLORS[] = {
    0x000000,   // 0: 空
    0xFF4757,   // 1: 樱桃红
    0xFFA502,   // 2: 亮金黄
    0x1E90FF,   // 3: 宝石蓝
    0x2ED573,   // 4: 翡翠绿
    0x9C27B0,   // 5: 葡萄紫
};
#define COLOR_COUNT 5

typedef struct {
    uint8_t color;
    lv_obj_t *obj;
    lv_obj_t *shine;
} bubble_cell_t;

typedef struct {
    bool active;
    float x, y;
    float vx, vy;
    uint8_t color;
    lv_obj_t *obj;
    lv_obj_t *shine;
} projectile_t;

typedef struct {
    bool active;
    float x, y;
    float vx, vy;
    int life;
    lv_obj_t *obj;
} particle_t;

// UI & Game Objects
static lv_obj_t *s_scr;
static lv_obj_t *s_score_label;
static lv_obj_t *s_level_label;
static lv_obj_t *s_ceiling_line;
static lv_obj_t *s_danger_line;
static lv_obj_t *s_cannon_base;
static lv_obj_t *s_current_bubble_obj;
static lv_obj_t *s_next_bubble_obj;
static lv_obj_t *s_aim_dots[5];
static lv_obj_t *s_game_over_panel;
static lv_obj_t *s_over_title_label;
static lv_obj_t *s_over_score_label;
static lv_timer_t *s_timer;

// Game State
static bubble_cell_t s_grid[GRID_ROWS][GRID_COLS];
static particle_t s_particles[MAX_PARTICLES];
static projectile_t s_bullet;
static uint32_t s_score = 0;
static uint8_t s_level = 1;
static float s_aim_angle = 0.0f; // degrees: -66 to +66
static uint8_t s_current_color = 1;
static uint8_t s_next_color = 2;
static int s_miss_count = 0;
static int s_ceiling_drop = 0;
static bool s_game_over = false;
static bool s_game_won = false;

static void update_score_display(void);
static void update_aim_guide(void);
static void create_bubble_visual(int r, int c);
static void destroy_bubble_visual(int r, int c);
static void spawn_pop_particles(float x, float y, uint32_t color);

static void get_cell_pos(int r, int c, float *out_x, float *out_y)
{
    float top_y = (float)(BOARD_TOP + s_ceiling_drop * ROW_HEIGHT);
    *out_y = top_y + (float)(r * ROW_HEIGHT) + (float)BUBBLE_RADIUS;

    if (r % 2 == 0) {
        *out_x = (float)(BOARD_LEFT + BUBBLE_RADIUS + c * BUBBLE_DIAMETER + 6);
    } else {
        *out_x = (float)(BOARD_LEFT + BUBBLE_RADIUS + c * BUBBLE_DIAMETER + 6 + BUBBLE_RADIUS);
    }
}

static void update_score_display(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "得分: %lu", (unsigned long)s_score);
    if (s_score_label) lv_label_set_text(s_score_label, buf);

    snprintf(buf, sizeof(buf), "关卡: %u", (unsigned)s_level);
    if (s_level_label) lv_label_set_text(s_level_label, buf);
}

static void update_launcher_visuals(void)
{
    if (s_current_bubble_obj) {
        lv_obj_set_style_bg_color(s_current_bubble_obj, lv_color_hex(BUBBLE_COLORS[s_current_color]), 0);
        if (s_bullet.active) {
            lv_obj_add_flag(s_current_bubble_obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_current_bubble_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_next_bubble_obj) {
        lv_obj_set_style_bg_color(s_next_bubble_obj, lv_color_hex(BUBBLE_COLORS[s_next_color]), 0);
    }
}

static void update_aim_guide(void)
{
    float rad = s_aim_angle * (float)M_PI / 180.0f;
    float dir_x = sinf(rad);
    float dir_y = -cosf(rad);

    float cur_x = (float)CANNON_X;
    float cur_y = (float)CANNON_Y;

    for (int i = 0; i < 5; i++) {
        cur_x += dir_x * 22.0f;
        cur_y += dir_y * 22.0f;

        // Bounce preview
        if (cur_x < (BOARD_LEFT + BUBBLE_RADIUS)) {
            cur_x = (BOARD_LEFT + BUBBLE_RADIUS) + ((BOARD_LEFT + BUBBLE_RADIUS) - cur_x);
            dir_x = -dir_x;
        } else if (cur_x > (BOARD_RIGHT - BUBBLE_RADIUS)) {
            cur_x = (BOARD_RIGHT - BUBBLE_RADIUS) - (cur_x - (BOARD_RIGHT - BUBBLE_RADIUS));
            dir_x = -dir_x;
        }

        if (s_aim_dots[i]) {
            lv_obj_set_pos(s_aim_dots[i], (int)cur_x - 3, (int)cur_y - 3);
        }
    }
}

static void create_bubble_visual(int r, int c)
{
    if (!s_scr || s_grid[r][c].obj || s_grid[r][c].color == 0) return;

    float x, y;
    get_cell_pos(r, c, &x, &y);

    lv_obj_t *obj = lv_obj_create(s_scr);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, BUBBLE_DIAMETER, BUBBLE_DIAMETER);
    lv_obj_set_pos(obj, (int)(x - BUBBLE_RADIUS), (int)(y - BUBBLE_RADIUS));
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(BUBBLE_COLORS[s_grid[r][c].color]), 0);
    lv_obj_set_style_border_color(obj, lv_color_white(), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);

    // Specular bubble highlight
    lv_obj_t *shine = lv_obj_create(obj);
    lv_obj_remove_flag(shine, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(shine, 5, 5);
    lv_obj_set_pos(shine, 4, 4);
    lv_obj_set_style_radius(shine, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(shine, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(shine, LV_OPA_80, 0);
    lv_obj_set_style_border_width(shine, 0, 0);

    s_grid[r][c].obj = obj;
    s_grid[r][c].shine = shine;
}

static void destroy_bubble_visual(int r, int c)
{
    if (s_grid[r][c].obj) {
        lv_obj_delete(s_grid[r][c].obj);
        s_grid[r][c].obj = NULL;
        s_grid[r][c].shine = NULL;
    }
}

static void refresh_all_bubble_positions(void)
{
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            if (s_grid[r][c].obj) {
                float x, y;
                get_cell_pos(r, c, &x, &y);
                lv_obj_set_pos(s_grid[r][c].obj, (int)(x - BUBBLE_RADIUS), (int)(y - BUBBLE_RADIUS));
            }
        }
    }
    if (s_ceiling_line) {
        lv_obj_set_y(s_ceiling_line, BOARD_TOP + s_ceiling_drop * ROW_HEIGHT);
    }
}

static void spawn_pop_particles(float x, float y, uint32_t color)
{
    for (int i = 0; i < 5; i++) {
        for (int p = 0; p < MAX_PARTICLES; p++) {
            if (!s_particles[p].active) {
                s_particles[p].active = true;
                s_particles[p].x = x;
                s_particles[p].y = y;
                float angle = (float)(rand() % 360) * (float)M_PI / 180.0f;
                float speed = 2.0f + (float)(rand() % 25) / 10.0f;
                s_particles[p].vx = cosf(angle) * speed;
                s_particles[p].vy = sinf(angle) * speed;
                s_particles[p].life = 10 + rand() % 6;

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

static void trigger_game_end(bool win)
{
    s_game_over = true;
    s_game_won = win;

    if (win) {
        game_audio_play(GAME_SFX_WIN);
    } else {
        game_audio_play(GAME_SFX_OVER);
    }

    if (!s_game_over_panel) {
        s_game_over_panel = ui_system_item_create(s_scr, 24, 90, 192, 140);
        lv_obj_set_style_border_width(s_game_over_panel, 3, 0);

        s_over_title_label = ui_system_label(s_game_over_panel, "", &ui_font_noto_sc_14, 0xFFFFFF);
        lv_obj_set_width(s_over_title_label, 192);
        lv_obj_set_style_text_align(s_over_title_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_over_title_label, 0, 16);

        s_over_score_label = ui_system_label(s_game_over_panel, "", &ui_font_noto_sc_14, 0xF1C40F);
        lv_obj_set_width(s_over_score_label, 192);
        lv_obj_set_style_text_align(s_over_score_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_over_score_label, 0, 50);

        lv_obj_t *hint = ui_system_label(s_game_over_panel, "按 OK 继续  长按返回", &ui_font_noto_sc_14, 0xBDC3C7);
        lv_obj_set_width(hint, 192);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(hint, 0, 95);
    }

    if (win) {
        lv_obj_set_style_bg_color(s_game_over_panel, lv_color_hex(0x1B5E20), 0);
        lv_obj_set_style_border_color(s_game_over_panel, lv_color_hex(0x4CAF50), 0);
        lv_label_set_text(s_over_title_label, "🎉 闯关成功！");
    } else {
        lv_obj_set_style_bg_color(s_game_over_panel, lv_color_hex(0x2C3E50), 0);
        lv_obj_set_style_border_color(s_game_over_panel, lv_color_hex(0xE74C3C), 0);
        lv_label_set_text(s_over_title_label, "游戏结束");
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "最终得分: %lu\n通关关卡: %u", (unsigned long)s_score, (unsigned)s_level);
    lv_label_set_text(s_over_score_label, buf);
    lv_obj_remove_flag(s_game_over_panel, LV_OBJ_FLAG_HIDDEN);
}

static void init_grid_level(uint8_t level)
{
    // Clear old bubbles
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            destroy_bubble_visual(r, c);
            s_grid[r][c].color = 0;
        }
    }

    s_ceiling_drop = 0;
    s_miss_count = 0;

    // Fill initial 4~5 rows
    int rows_to_fill = 4 + (level > 2 ? 1 : 0);
    int num_colors = (level <= 1) ? 3 : ((level <= 3) ? 4 : 5);

    for (int r = 0; r < rows_to_fill; r++) {
        int max_cols = (r % 2 == 0) ? 8 : 7;
        for (int c = 0; c < max_cols; c++) {
            uint8_t color = 1 + (rand() % num_colors);
            s_grid[r][c].color = color;
            create_bubble_visual(r, c);
        }
    }

    s_current_color = 1 + (rand() % num_colors);
    s_next_color = 1 + (rand() % num_colors);
    update_launcher_visuals();
    refresh_all_bubble_positions();
}

static void bubble_reset_game(void)
{
    s_score = 0;
    s_level = 1;
    s_aim_angle = 0.0f;
    s_game_over = false;
    s_game_won = false;
    s_bullet.active = false;

    if (s_bullet.obj) {
        lv_obj_add_flag(s_bullet.obj, LV_OBJ_FLAG_HIDDEN);
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

    init_grid_level(s_level);
    update_score_display();
    update_aim_guide();
}

// Hex neighbor lookup offsets
static const int NEIGHBORS_EVEN[6][2] = {
    { 0, -1 }, { 0, 1 }, { -1, -1 }, { -1, 0 }, { 1, -1 }, { 1, 0 }
};
static const int NEIGHBORS_ODD[6][2] = {
    { 0, -1 }, { 0, 1 }, { -1, 0 }, { -1, 1 }, { 1, 0 }, { 1, 1 }
};

static void get_neighbors(int r, int c, int out_r[6], int out_c[6], int *count)
{
    *count = 0;
    const int (*nb)[2] = (r % 2 == 0) ? NEIGHBORS_EVEN : NEIGHBORS_ODD;
    for (int i = 0; i < 6; i++) {
        int nr = r + nb[i][0];
        int nc = c + nb[i][1];
        int max_cols = (nr % 2 == 0) ? 8 : 7;
        if (nr >= 0 && nr < GRID_ROWS && nc >= 0 && nc < max_cols) {
            out_r[*count] = nr;
            out_c[*count] = nc;
            (*count)++;
        }
    }
}

// BFS to find matching color cluster
static void check_matches_and_drop(int hit_r, int hit_c)
{
    uint8_t match_color = s_grid[hit_r][hit_c].color;
    if (match_color == 0) return;

    bool visited[GRID_ROWS][GRID_COLS];
    memset(visited, 0, sizeof(visited));

    int queue_r[GRID_ROWS * GRID_COLS];
    int queue_c[GRID_ROWS * GRID_COLS];
    int head = 0, tail = 0;

    queue_r[tail] = hit_r;
    queue_c[tail] = hit_c;
    tail++;
    visited[hit_r][hit_c] = true;

    while (head < tail) {
        int cr = queue_r[head];
        int cc = queue_c[head];
        head++;

        int nr[6], nc[6], count;
        get_neighbors(cr, cc, nr, nc, &count);
        for (int i = 0; i < count; i++) {
            int r = nr[i];
            int c = nc[i];
            if (!visited[r][c] && s_grid[r][c].color == match_color) {
                visited[r][c] = true;
                queue_r[tail] = r;
                queue_c[tail] = c;
                tail++;
            }
        }
    }

    if (tail >= 3) {
        // Pop matching bubbles!
        for (int i = 0; i < tail; i++) {
            int r = queue_r[i];
            int c = queue_c[i];
            float x, y;
            get_cell_pos(r, c, &x, &y);
            spawn_pop_particles(x, y, BUBBLE_COLORS[s_grid[r][c].color]);
            destroy_bubble_visual(r, c);
            s_grid[r][c].color = 0;
        }

        s_score += (uint32_t)tail * 10;
        if (tail >= 5) {
            game_audio_play(GAME_SFX_COMBO);
        } else {
            game_audio_play(GAME_SFX_POP);
        }

        // Drop isolated / floating bubbles (unconnected to ceiling row 0)
        bool anchored[GRID_ROWS][GRID_COLS];
        memset(anchored, 0, sizeof(anchored));
        head = 0; tail = 0;

        for (int c = 0; c < 8; c++) {
            if (s_grid[0][c].color != 0) {
                anchored[0][c] = true;
                queue_r[tail] = 0;
                queue_c[tail] = c;
                tail++;
            }
        }

        while (head < tail) {
            int cr = queue_r[head];
            int cc = queue_c[head];
            head++;

            int nr[6], nc[6], count;
            get_neighbors(cr, cc, nr, nc, &count);
            for (int i = 0; i < count; i++) {
                int r = nr[i];
                int c = nc[i];
                if (!anchored[r][c] && s_grid[r][c].color != 0) {
                    anchored[r][c] = true;
                    queue_r[tail] = r;
                    queue_c[tail] = c;
                    tail++;
                }
            }
        }

        // Any unanchored bubbles fall
        int dropped_count = 0;
        for (int r = 0; r < GRID_ROWS; r++) {
            int max_cols = (r % 2 == 0) ? 8 : 7;
            for (int c = 0; c < max_cols; c++) {
                if (s_grid[r][c].color != 0 && !anchored[r][c]) {
                    float x, y;
                    get_cell_pos(r, c, &x, &y);
                    spawn_pop_particles(x, y, BUBBLE_COLORS[s_grid[r][c].color]);
                    destroy_bubble_visual(r, c);
                    s_grid[r][c].color = 0;
                    dropped_count++;
                }
            }
        }

        if (dropped_count > 0) {
            s_score += (uint32_t)dropped_count * 25;
            game_audio_play(GAME_SFX_MERGE_MED);
        }

        update_score_display();

        // Check if all cleared (Win!)
        bool any_left = false;
        for (int r = 0; r < GRID_ROWS; r++) {
            for (int c = 0; c < GRID_COLS; c++) {
                if (s_grid[r][c].color != 0) {
                    any_left = true;
                    break;
                }
            }
            if (any_left) break;
        }

        if (!any_left) {
            s_score += 500 * s_level;
            update_score_display();
            trigger_game_end(true);
        }
    } else {
        // Miss! Increment miss counter
        s_miss_count++;
        if (s_miss_count >= 5) {
            s_miss_count = 0;
            s_ceiling_drop++;
            refresh_all_bubble_positions();

            // Check if ceiling pushed bubbles across danger line
            for (int r = 0; r < GRID_ROWS; r++) {
                for (int c = 0; c < GRID_COLS; c++) {
                    if (s_grid[r][c].color != 0) {
                        float bx, by;
                        get_cell_pos(r, c, &bx, &by);
                        if (by + BUBBLE_RADIUS >= 256.0f) {
                            trigger_game_end(false);
                            return;
                        }
                    }
                }
            }
        }
    }
}

static void attach_bullet(float hit_x, float hit_y)
{
    // Find closest valid empty cell
    int best_r = -1, best_c = -1;
    float best_dist = 99999.0f;

    for (int r = 0; r < GRID_ROWS; r++) {
        int max_cols = (r % 2 == 0) ? 8 : 7;
        for (int c = 0; c < max_cols; c++) {
            if (s_grid[r][c].color == 0) {
                float cx, cy;
                get_cell_pos(r, c, &cx, &cy);
                float dist = (hit_x - cx) * (hit_x - cx) + (hit_y - cy) * (hit_y - cy);
                if (dist < best_dist) {
                    best_dist = dist;
                    best_r = r;
                    best_c = c;
                }
            }
        }
    }

    if (best_r >= 0 && best_c >= 0) {
        s_grid[best_r][best_c].color = s_bullet.color;
        create_bubble_visual(best_r, best_c);

        // Check if attached bubble crossed danger line
        float bx, by;
        get_cell_pos(best_r, best_c, &bx, &by);
        if (by + BUBBLE_RADIUS >= 256.0f) {
            trigger_game_end(false);
            return;
        }

        check_matches_and_drop(best_r, best_c);
    }

    s_bullet.active = false;
    if (s_bullet.obj) {
        lv_obj_add_flag(s_bullet.obj, LV_OBJ_FLAG_HIDDEN);
    }

    s_current_color = s_next_color;
    s_next_color = 1 + (rand() % COLOR_COUNT);
    update_launcher_visuals();
}

static void bubble_physics_step(void)
{
    if (s_game_over) return;

    // 1. Bullet projectile movement
    if (s_bullet.active) {
        s_bullet.x += s_bullet.vx;
        s_bullet.y += s_bullet.vy;

        // Side wall bounce
        if (s_bullet.x < (BOARD_LEFT + BUBBLE_RADIUS)) {
            s_bullet.x = (BOARD_LEFT + BUBBLE_RADIUS);
            s_bullet.vx = -s_bullet.vx;
        } else if (s_bullet.x > (BOARD_RIGHT - BUBBLE_RADIUS)) {
            s_bullet.x = (BOARD_RIGHT - BUBBLE_RADIUS);
            s_bullet.vx = -s_bullet.vx;
        }

        // Ceiling collision
        float top_y = (float)(BOARD_TOP + s_ceiling_drop * ROW_HEIGHT) + (float)BUBBLE_RADIUS;
        if (s_bullet.y <= top_y) {
            attach_bullet(s_bullet.x, top_y);
        } else {
            // Check collision with existing bubbles
            bool collided = false;
            for (int r = 0; r < GRID_ROWS && !collided; r++) {
                int max_cols = (r % 2 == 0) ? 8 : 7;
                for (int c = 0; c < max_cols; c++) {
                    if (s_grid[r][c].color != 0) {
                        float bx, by;
                        get_cell_pos(r, c, &bx, &by);
                        float dx = s_bullet.x - bx;
                        float dy = s_bullet.y - by;
                        float dist = sqrtf(dx * dx + dy * dy);
                        if (dist < (BUBBLE_DIAMETER - 2)) {
                            attach_bullet(s_bullet.x, s_bullet.y);
                            collided = true;
                            break;
                        }
                    }
                }
            }
        }

        if (s_bullet.active && s_bullet.obj) {
            lv_obj_set_pos(s_bullet.obj, (int)(s_bullet.x - BUBBLE_RADIUS), (int)(s_bullet.y - BUBBLE_RADIUS));
        }
    }

    // 2. Particles update
    for (int p = 0; p < MAX_PARTICLES; p++) {
        if (!s_particles[p].active) continue;

        s_particles[p].x += s_particles[p].vx;
        s_particles[p].y += s_particles[p].vy;
        s_particles[p].vy += 0.25f;
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
}

static void bubble_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    bubble_physics_step();
}

static void launch_bubble(void)
{
    if (s_bullet.active || s_game_over) return;

    game_audio_play(GAME_SFX_SHOOT);

    float rad = s_aim_angle * (float)M_PI / 180.0f;
    float speed = 8.5f;

    s_bullet.active = true;
    s_bullet.x = (float)CANNON_X;
    s_bullet.y = (float)CANNON_Y;
    s_bullet.vx = sinf(rad) * speed;
    s_bullet.vy = -cosf(rad) * speed;
    s_bullet.color = s_current_color;

    if (!s_bullet.obj) {
        s_bullet.obj = lv_obj_create(s_scr);
        lv_obj_remove_flag(s_bullet.obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_bullet.obj, BUBBLE_DIAMETER, BUBBLE_DIAMETER);
        lv_obj_set_style_radius(s_bullet.obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(s_bullet.obj, lv_color_white(), 0);
        lv_obj_set_style_border_width(s_bullet.obj, 1, 0);
        lv_obj_set_style_pad_all(s_bullet.obj, 0, 0);

        s_bullet.shine = lv_obj_create(s_bullet.obj);
        lv_obj_remove_flag(s_bullet.shine, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_bullet.shine, 5, 5);
        lv_obj_set_pos(s_bullet.shine, 4, 4);
        lv_obj_set_style_radius(s_bullet.shine, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_bullet.shine, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_bullet.shine, LV_OPA_80, 0);
        lv_obj_set_style_border_width(s_bullet.shine, 0, 0);
    }

    lv_obj_set_style_bg_color(s_bullet.obj, lv_color_hex(BUBBLE_COLORS[s_bullet.color]), 0);
    lv_obj_remove_flag(s_bullet.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_bullet.obj, (int)(s_bullet.x - BUBBLE_RADIUS), (int)(s_bullet.y - BUBBLE_RADIUS));

    update_launcher_visuals();
}

void demo_game_bubble_enter(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x232931), 0); // Dark sleek candy arcade aesthetic

    // Board container border
    lv_obj_t *border = lv_obj_create(s_scr);
    lv_obj_remove_flag(border, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(border, BOARD_LEFT - 2, BOARD_TOP - 2);
    lv_obj_set_size(border, (BOARD_RIGHT - BOARD_LEFT) + 4, 276);
    lv_obj_set_style_radius(border, 12, 0);
    lv_obj_set_style_bg_color(border, lv_color_hex(0x181C22), 0);
    lv_obj_set_style_border_color(border, lv_color_hex(0x393E46), 0);
    lv_obj_set_style_border_width(border, 2, 0);
    lv_obj_set_style_pad_all(border, 0, 0);

    // Ceiling bar
    s_ceiling_line = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_ceiling_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_ceiling_line, BOARD_LEFT, BOARD_TOP);
    lv_obj_set_size(s_ceiling_line, BOARD_RIGHT - BOARD_LEFT, 3);
    lv_obj_set_style_bg_color(s_ceiling_line, lv_color_hex(0xEEEEEE), 0);
    lv_obj_set_style_border_width(s_ceiling_line, 0, 0);

    // Danger line at bottom
    s_danger_line = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_danger_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_danger_line, BOARD_LEFT, 256);
    lv_obj_set_size(s_danger_line, BOARD_RIGHT - BOARD_LEFT, 2);
    lv_obj_set_style_bg_color(s_danger_line, lv_color_hex(0xFF4757), 0);
    lv_obj_set_style_bg_opa(s_danger_line, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_danger_line, 0, 0);

    // Header: Score & Level
    s_score_label = ui_system_label(s_scr, "得分: 0", &ui_font_noto_sc_14, 0x00ADB5);
    lv_obj_set_pos(s_score_label, 14, 8);

    s_level_label = ui_system_label(s_scr, "关卡: 1", &ui_font_noto_sc_14, 0xEEEEEE);
    lv_obj_set_pos(s_level_label, 172, 8);

    // Aim Guide Dots
    for (int i = 0; i < 5; i++) {
        s_aim_dots[i] = lv_obj_create(s_scr);
        lv_obj_remove_flag(s_aim_dots[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_aim_dots[i], 4, 4);
        lv_obj_set_style_radius(s_aim_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_aim_dots[i], lv_color_hex(0x00ADB5), 0);
        lv_obj_set_style_border_width(s_aim_dots[i], 0, 0);
    }

    // Cannon Base Platform
    s_cannon_base = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_cannon_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_cannon_base, 36, 36);
    lv_obj_set_pos(s_cannon_base, CANNON_X - 18, CANNON_Y - 18);
    lv_obj_set_style_radius(s_cannon_base, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_cannon_base, lv_color_hex(0x393E46), 0);
    lv_obj_set_style_border_color(s_cannon_base, lv_color_hex(0x00ADB5), 0);
    lv_obj_set_style_border_width(s_cannon_base, 2, 0);

    // Current Bubble in launcher
    s_current_bubble_obj = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_current_bubble_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_current_bubble_obj, BUBBLE_DIAMETER, BUBBLE_DIAMETER);
    lv_obj_set_pos(s_current_bubble_obj, CANNON_X - BUBBLE_RADIUS, CANNON_Y - BUBBLE_RADIUS);
    lv_obj_set_style_radius(s_current_bubble_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_current_bubble_obj, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_current_bubble_obj, 1, 0);

    // Next Bubble Preview
    s_next_bubble_obj = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_next_bubble_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_next_bubble_obj, 16, 16);
    lv_obj_set_pos(s_next_bubble_obj, CANNON_X + 28, CANNON_Y - 8);
    lv_obj_set_style_radius(s_next_bubble_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_next_bubble_obj, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_next_bubble_obj, 1, 0);

    s_game_over_panel = NULL;
    s_bullet.obj = NULL;

    bubble_reset_game();

    s_timer = lv_timer_create(bubble_timer_cb, TICK_PERIOD_MS, NULL);
    lv_screen_load(s_scr);
}

void demo_game_bubble_exit(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            destroy_bubble_visual(r, c);
            s_grid[r][c].color = 0;
        }
    }
    for (int p = 0; p < MAX_PARTICLES; p++) {
        if (s_particles[p].obj) {
            lv_obj_delete(s_particles[p].obj);
            s_particles[p].obj = NULL;
        }
        s_particles[p].active = false;
    }
    if (s_bullet.obj) {
        lv_obj_delete(s_bullet.obj);
        s_bullet.obj = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_score_label = NULL;
        s_level_label = NULL;
        s_ceiling_line = NULL;
        s_danger_line = NULL;
        s_cannon_base = NULL;
        s_current_bubble_obj = NULL;
        s_next_bubble_obj = NULL;
        for (int i = 0; i < 5; i++) s_aim_dots[i] = NULL;
        s_game_over_panel = NULL;
    }
}

void demo_game_bubble_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (s_game_over) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            game_audio_play(GAME_SFX_POP);
            if (s_game_won) {
                s_level++;
                s_game_over = false;
                s_game_won = false;
                if (s_game_over_panel) {
                    lv_obj_add_flag(s_game_over_panel, LV_OBJ_FLAG_HIDDEN);
                }
                init_grid_level(s_level);
                update_score_display();
            } else {
                bubble_reset_game();
            }
        }
        return;
    }

    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) {
            launch_bubble();
        }
        return;
    }

    // UP/DOWN 按键支持单击、长按开始、以及长按连续连发 (HOLD)
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG && ev != BSP_BTN_HOLD) return;

    if (btn == BSP_BTN_UP) {
        // Rotate Counter-Clockwise (Left)
        s_aim_angle -= 5.0f;
        if (s_aim_angle < -66.0f) s_aim_angle = -66.0f;
        game_audio_play(GAME_SFX_MOVE);
        update_aim_guide();
    } else if (btn == BSP_BTN_DOWN) {
        // Rotate Clockwise (Right)
        s_aim_angle += 5.0f;
        if (s_aim_angle > 66.0f) s_aim_angle = 66.0f;
        game_audio_play(GAME_SFX_MOVE);
        update_aim_guide();
    }
}
