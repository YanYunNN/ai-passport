// main/demo_game_tetris.c - 《俄罗斯方块》(Tetris)
// 三键方案: 上/下 = 左右移动(按下即走,长按连发), OK 单击 = 旋转, OK 双击 = 速降。
// 长按 OK 由 main 拦截返回游戏菜单, 这里不处理。
// 内存约束: 板内格子用懒建对象(最多 ROWS*COLS 个), 与泡泡龙的格子量级相当,
// 保持在 CONFIG_LV_MEM_SIZE_KILOBYTES=32 的 LVGL 对象池内。
#include "demo_game_tetris.h"
#include "game_audio.h"
#include "ui_system.h"
#include "ui_font_noto_sc_14.h"
#include "bsp_button.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS            10
#define ROWS            18
#define CELL            15      // 放大网格, 游戏区更开阔饱满 (10 * 15 = 150px)
#define BOARD_LEFT      10
#define BOARD_TOP       32
#define TICK_PERIOD_MS  20

// 7 种方块的基础形态: 16 位掩码表示 4x4 网格, bit15=(0,0), 行内高位在左。
// 只存第一朝向, 其余朝向在运行时旋转掩码得到。
static const uint16_t PIECE_BASE[7] = {
    0x0F00, // I
    0x0660, // O
    0x04E0, // T
    0x06C0, // S
    0x0C60, // Z
    0x08E0, // J
    0x02E0, // L
};

// 索引 1..7 对应方块类型 0..6
static const uint32_t PIECE_COLORS[8] = {
    0x000000, 0x00D2D3, 0xF1C40F, 0x9B59B6,
    0x2ECC71, 0xFF4757, 0x3498DB, 0xE67E22,
};

typedef struct {
    uint8_t type; // 0..6
    uint8_t rot;  // 0..3
    int8_t  x;    // 网格列(4x4 盒左上角)
    int8_t  y;    // 网格行(4x4 盒左上角)
} piece_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_score_label;
static lv_obj_t *s_high_label;
static lv_obj_t *s_level_label;
static lv_obj_t *s_next_preview[16];
static lv_obj_t *s_cells[ROWS][COLS];
static lv_timer_t *s_timer;
static lv_obj_t *s_game_over_panel;
static lv_obj_t *s_over_score_label;

static uint8_t s_board[ROWS][COLS];  // 0=空, 1..7=颜色索引
static uint32_t s_drawn[ROWS][COLS]; // 当前已画出的颜色, 0=隐藏
static piece_t s_cur;
static uint8_t s_next_type;
static uint32_t s_score;
static uint32_t s_high_score;
static int s_lines;
static int s_level;
static int s_fall_accum;
static bool s_game_over;

static uint16_t rotate_mask_cw(uint16_t m)
{
    uint16_t out = 0;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (m & (1u << (15 - (r * 4 + c)))) {
                out |= (1u << (15 - (c * 4 + (3 - r))));
            }
        }
    }
    return out;
}

static uint16_t piece_mask(const piece_t *p)
{
    uint16_t m = PIECE_BASE[p->type];
    for (int i = 0; i < p->rot; i++) {
        m = rotate_mask_cw(m);
    }
    return m;
}

// 检测方块在偏移 (dx,dy) 处是否与墙或已落定方块碰撞。
static bool collides(const piece_t *p, int dx, int dy)
{
    uint16_t m = piece_mask(p);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!(m & (1u << (15 - (r * 4 + c))))) continue;
            int br = p->y + r + dy;
            int bc = p->x + c + dx;
            if (bc < 0 || bc >= COLS || br >= ROWS) return true;
            if (br >= 0 && s_board[br][bc]) return true;
        }
    }
    return false;
}

// 把方块写进/清出棋盘(只写棋盘范围内)
static void board_fill(const piece_t *p, uint8_t v)
{
    uint16_t m = piece_mask(p);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!(m & (1u << (15 - (r * 4 + c))))) continue;
            int br = p->y + r;
            int bc = p->x + c;
            if (br >= 0 && br < ROWS && bc >= 0 && bc < COLS) s_board[br][bc] = v;
        }
    }
}

static lv_obj_t *cell_obj(int r, int c)
{
    if (s_cells[r][c]) return s_cells[r][c];
    lv_obj_t *o = lv_obj_create(s_scr);
    if (!o) return NULL; // LVGL 对象池耗尽时不崩溃
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, BOARD_LEFT + c * CELL, BOARD_TOP + r * CELL);
    lv_obj_set_size(o, CELL - 1, CELL - 1);
    lv_obj_set_style_radius(o, 2, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    s_cells[r][c] = o;
    return o;
}

// 刷新棋盘渲染
static void sync_board(void)
{
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            uint32_t want = PIECE_COLORS[s_board[r][c]];
            if (want == s_drawn[r][c]) continue;
            s_drawn[r][c] = want;
            if (want == 0) {
                if (s_cells[r][c]) lv_obj_add_flag(s_cells[r][c], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_t *o = cell_obj(r, c);
                if (!o) continue;
                lv_obj_set_style_bg_color(o, lv_color_hex(want), 0);
                lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void update_hud(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)s_score);
    if (s_score_label) lv_label_set_text(s_score_label, buf);
    if (s_score > s_high_score) s_high_score = s_score;
    snprintf(buf, sizeof(buf), "最高:%lu", (unsigned long)s_high_score);
    if (s_high_label) lv_label_set_text(s_high_label, buf);
    snprintf(buf, sizeof(buf), "第 %d 关", s_level);
    if (s_level_label) lv_label_set_text(s_level_label, buf);
}

static void update_next_preview(void)
{
    for (int i = 0; i < 16; i++) {
        if (s_next_preview[i]) lv_obj_add_flag(s_next_preview[i], LV_OBJ_FLAG_HIDDEN);
    }
    uint16_t m = PIECE_BASE[s_next_type];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!(m & (1u << (15 - (r * 4 + c))))) continue;
            int idx = r * 4 + c;
            if (!s_next_preview[idx]) {
                s_next_preview[idx] = lv_obj_create(s_scr);
                if (!s_next_preview[idx]) continue;
                lv_obj_remove_flag(s_next_preview[idx], LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_size(s_next_preview[idx], 8, 8);
                lv_obj_set_style_radius(s_next_preview[idx], 1, 0);
                lv_obj_set_style_border_width(s_next_preview[idx], 0, 0);
                lv_obj_set_style_pad_all(s_next_preview[idx], 0, 0);
            }
            // 右侧紧凑预览区: x: 180 + c*9, y: 56 + r*9
            lv_obj_set_pos(s_next_preview[idx], 180 + c * 9, 56 + r * 9);
            lv_obj_set_style_bg_color(s_next_preview[idx],
                                      lv_color_hex(PIECE_COLORS[s_next_type + 1]), 0);
            lv_obj_remove_flag(s_next_preview[idx], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void trigger_game_over(void)
{
    s_game_over = true;
    if (s_score > s_high_score) s_high_score = s_score;
    game_audio_play(GAME_SFX_OVER);
    sync_board();

    if (!s_game_over_panel) {
        s_game_over_panel = ui_system_item_create(s_scr, 24, 96, 192, 140);
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
    snprintf(buf, sizeof(buf), "本次得分: %lu\n最高得分: %lu",
             (unsigned long)s_score, (unsigned long)s_high_score);
    lv_label_set_text(s_over_score_label, buf);
    lv_obj_remove_flag(s_game_over_panel, LV_OBJ_FLAG_HIDDEN);
}

static void spawn_piece(void)
{
    s_cur.type = s_next_type;
    s_cur.rot = 0;
    s_cur.x = 3;
    s_cur.y = 0;
    s_next_type = rand() % 7;
    update_next_preview();
    if (collides(&s_cur, 0, 0)) {
        trigger_game_over();
        return;
    }
    board_fill(&s_cur, (uint8_t)(s_cur.type + 1));
    sync_board();
}

static void lock_piece(void)
{
    int cleared = 0;
    int write_row = ROWS - 1;

    // 采用稳定可靠的单趟双指针扫描消除满行，彻底杜绝死循环
    for (int r = ROWS - 1; r >= 0; r--) {
        bool full = true;
        for (int c = 0; c < COLS; c++) {
            if (!s_board[r][c]) {
                full = false;
                break;
            }
        }
        if (full) {
            cleared++;
        } else {
            if (write_row != r) {
                memcpy(s_board[write_row], s_board[r], COLS);
            }
            write_row--;
        }
    }

    // 顶部腾出的行全部清零
    while (write_row >= 0) {
        memset(s_board[write_row], 0, COLS);
        write_row--;
    }

    if (cleared > 0) {
        if (cleared > 4) cleared = 4;
        static const uint32_t LINE_SCORE[] = { 100, 300, 500, 800 };
        s_score += LINE_SCORE[cleared - 1] * (uint32_t)s_level;
        s_lines += cleared;
        int new_level = s_lines / 10 + 1;
        if (new_level != s_level) s_level = new_level;
        if (cleared >= 4) game_audio_play(GAME_SFX_MERGE_BIG);
        else if (cleared >= 2) game_audio_play(GAME_SFX_COMBO);
        else game_audio_play(GAME_SFX_POP);
        update_hud();
    }
    spawn_piece();
}

static void tetris_reset(void)
{
    memset(s_board, 0, sizeof(s_board));
    s_score = 0;
    s_lines = 0;
    s_level = 1;
    s_fall_accum = 0;
    s_game_over = false;
    s_cur.type = rand() % 7;
    s_cur.rot = 0;
    s_cur.x = 3;
    s_cur.y = 0;
    s_next_type = rand() % 7;
    if (s_game_over_panel) lv_obj_add_flag(s_game_over_panel, LV_OBJ_FLAG_HIDDEN);
    update_next_preview();
    update_hud();
    board_fill(&s_cur, (uint8_t)(s_cur.type + 1));
    sync_board();
}

static void tetris_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_game_over) return;

    s_fall_accum += TICK_PERIOD_MS;
    int period = 600 - (s_level - 1) * 50;
    if (period < 80) period = 80;
    if (s_fall_accum < period) return;
    s_fall_accum -= period;

    board_fill(&s_cur, 0);
    if (!collides(&s_cur, 0, 1)) {
        s_cur.y++;
        board_fill(&s_cur, (uint8_t)(s_cur.type + 1));
        sync_board();
    } else {
        board_fill(&s_cur, (uint8_t)(s_cur.type + 1));
        lock_piece();
    }
}

static const int8_t KICK_DX[] = { 0, -1, 1, -2, 2, -1, 1, -2, 2, 0 };
static const int8_t KICK_DY[] = { 0,  0, 0,  0, 0, -1, -1, -1, -1, -2 };
#define KICK_COUNT (sizeof(KICK_DX) / sizeof(KICK_DX[0]))

static bool rotate_piece(void)
{
    if (s_cur.type == 1) return false;

    board_fill(&s_cur, 0);
    piece_t n = s_cur;
    n.rot = (s_cur.rot + 1) & 3;
    bool ok = false;
    for (size_t i = 0; i < KICK_COUNT && !ok; i++) {
        n.x = s_cur.x + KICK_DX[i];
        n.y = s_cur.y + KICK_DY[i];
        if (n.y < 0) n.y = 0;
        if (!collides(&n, 0, 0)) ok = true;
    }
    if (ok) s_cur = n;
    board_fill(&s_cur, (uint8_t)(s_cur.type + 1));
    sync_board();
    return ok;
}

static void move_piece_h(int dx)
{
    board_fill(&s_cur, 0);
    s_cur.x += dx;
    if (collides(&s_cur, 0, 0)) s_cur.x -= dx;
    board_fill(&s_cur, (uint8_t)(s_cur.type + 1));
    sync_board();
}

static void hard_drop(void)
{
    board_fill(&s_cur, 0);
    int drop_limit = 0;
    while (!collides(&s_cur, 0, 1) && drop_limit < ROWS) {
        s_cur.y++;
        drop_limit++;
    }
    board_fill(&s_cur, (uint8_t)(s_cur.type + 1));
    game_audio_play(GAME_SFX_DROP);
    lock_piece();
}

void demo_game_tetris_enter(void)
{
    s_scr = ui_system_screen_create();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x10131A), 0);

    // 1. 游戏棋盘背景板 (宽阔 150px 宽, 270px 高)
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

    // 顶部简明标题
    lv_obj_t *top_title = ui_system_label(s_scr, "俄罗斯方块", &ui_font_noto_sc_14, 0x58D68D);
    lv_obj_set_pos(top_title, 10, 10);

    // 2. 右侧紧凑信息面板 (x: 168, w: 64)
    // 下一个方块卡片
    lv_obj_t *next_card = lv_obj_create(s_scr);
    if (next_card) {
        lv_obj_remove_flag(next_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(next_card, 168, 32);
        lv_obj_set_size(next_card, 64, 66);
        lv_obj_set_style_radius(next_card, 6, 0);
        lv_obj_set_style_bg_color(next_card, lv_color_hex(0x1B2028), 0);
        lv_obj_set_style_border_color(next_card, lv_color_hex(0x3A4150), 0);
        lv_obj_set_style_border_width(next_card, 1, 0);
        lv_obj_set_style_pad_all(next_card, 0, 0);

        lv_obj_t *lbl = ui_system_label(next_card, "下一个", &ui_font_noto_sc_14, 0xA6ABB5);
        lv_obj_set_width(lbl, 64);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(lbl, 0, 4);
    }

    // 得分卡片
    lv_obj_t *score_card = lv_obj_create(s_scr);
    if (score_card) {
        lv_obj_remove_flag(score_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(score_card, 168, 104);
        lv_obj_set_size(score_card, 64, 86);
        lv_obj_set_style_radius(score_card, 6, 0);
        lv_obj_set_style_bg_color(score_card, lv_color_hex(0x1B2028), 0);
        lv_obj_set_style_border_color(score_card, lv_color_hex(0x3A4150), 0);
        lv_obj_set_style_border_width(score_card, 1, 0);
        lv_obj_set_style_pad_all(score_card, 0, 0);

        lv_obj_t *s_title = ui_system_label(score_card, "得分", &ui_font_noto_sc_14, 0xA6ABB5);
        lv_obj_set_width(s_title, 64);
        lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_title, 0, 4);

        s_score_label = ui_system_label(score_card, "0", &ui_font_noto_sc_14, 0xF1C40F);
        lv_obj_set_width(s_score_label, 64);
        lv_obj_set_style_text_align(s_score_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_score_label, 0, 22);

        s_level_label = ui_system_label(score_card, "第 1 关", &ui_font_noto_sc_14, 0x2ECC71);
        lv_obj_set_width(s_level_label, 64);
        lv_obj_set_style_text_align(s_level_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_level_label, 0, 44);

        s_high_label = ui_system_label(score_card, "最高:0", &ui_font_noto_sc_14, 0x7F8C8D);
        lv_obj_set_width(s_high_label, 64);
        lv_obj_set_style_text_align(s_high_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_high_label, 0, 64);
    }

    // 按键提示卡片
    lv_obj_t *hint_card = lv_obj_create(s_scr);
    if (hint_card) {
        lv_obj_remove_flag(hint_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(hint_card, 168, 196);
        lv_obj_set_size(hint_card, 64, 106);
        lv_obj_set_style_radius(hint_card, 6, 0);
        lv_obj_set_style_bg_color(hint_card, lv_color_hex(0x1B2028), 0);
        lv_obj_set_style_border_color(hint_card, lv_color_hex(0x3A4150), 0);
        lv_obj_set_style_border_width(hint_card, 1, 0);
        lv_obj_set_style_pad_all(hint_card, 0, 0);

        lv_obj_t *h1 = ui_system_label(hint_card, "上下:移动", &ui_font_noto_sc_14, 0xA6ABB5);
        lv_obj_set_pos(h1, 4, 6);
        lv_obj_t *h2 = ui_system_label(hint_card, "OK:旋转", &ui_font_noto_sc_14, 0xA6ABB5);
        lv_obj_set_pos(h2, 4, 28);
        lv_obj_t *h3 = ui_system_label(hint_card, "双击:速降", &ui_font_noto_sc_14, 0xA6ABB5);
        lv_obj_set_pos(h3, 4, 52);
        lv_obj_t *h4 = ui_system_label(hint_card, "长按:返回", &ui_font_noto_sc_14, 0x7F8C8D);
        lv_obj_set_pos(h4, 4, 76);
    }

    s_game_over_panel = NULL;
    s_over_score_label = NULL;
    memset(s_drawn, 0, sizeof(s_drawn));
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            s_cells[r][c] = NULL;
        }
    }
    for (int i = 0; i < 16; i++) s_next_preview[i] = NULL;

    tetris_reset();
    s_timer = lv_timer_create(tetris_tick_cb, TICK_PERIOD_MS, NULL);
    lv_screen_load(s_scr);
}

void demo_game_tetris_exit(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            s_cells[r][c] = NULL;
        }
    }
    for (int i = 0; i < 16; i++) s_next_preview[i] = NULL;
    s_score_label = NULL;
    s_high_label = NULL;
    s_level_label = NULL;
    s_game_over_panel = NULL;
    s_over_score_label = NULL;
    memset(s_board, 0, sizeof(s_board));
    memset(s_drawn, 0, sizeof(s_drawn));
}

void demo_game_tetris_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (s_game_over) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            game_audio_play(GAME_SFX_POP);
            tetris_reset();
        }
        return;
    }

    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) {
            if (rotate_piece()) game_audio_play(GAME_SFX_MOVE);
        } else if (ev == BSP_BTN_DOUBLE) {
            hard_drop();
        }
        return;
    }

    // 上/下 = 左右移动: 按下即走(低延迟), 长按连发
    if (ev != BSP_BTN_PRESS && ev != BSP_BTN_HOLD) return;
    move_piece_h(btn == BSP_BTN_UP ? -1 : 1);
    game_audio_play(GAME_SFX_MOVE);
}
