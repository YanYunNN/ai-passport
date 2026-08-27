// main/demo_games.c - 游戏板块子菜单（合成大西瓜、泡泡龙、俄罗斯方块、贪吃蛇、像素小鸟）
#include "demo_games.h"
#include "demo_game_suika.h"
#include "demo_game_bubble.h"
#include "demo_game_tetris.h"
#include "demo_game_snake.h"
#include "demo_game_flappy.h"
#include "game_audio.h"
#include "ui_system.h"
#include "ui_status.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "bsp_button.h"
#include "lvgl.h"

typedef struct {
    const char *name;
    const char *desc;
    void (*enter)(void);
    void (*exit)(void);
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);
} game_item_t;

static const game_item_t GAMES[] = {
    { "合成大西瓜", "Suika", demo_game_suika_enter, demo_game_suika_exit, demo_game_suika_key },
    { "泡泡龙",     "Bobble", demo_game_bubble_enter, demo_game_bubble_exit, demo_game_bubble_key },
    { "俄罗斯方块", "Tetris", demo_game_tetris_enter, demo_game_tetris_exit, demo_game_tetris_key },
    { "贪吃蛇",     "Snake", demo_game_snake_enter, demo_game_snake_exit, demo_game_snake_key },
    { "像素小鸟",   "Flappy", demo_game_flappy_enter, demo_game_flappy_exit, demo_game_flappy_key },
};

#define GAME_COUNT (sizeof(GAMES) / sizeof(GAMES[0]))
#define MENU_ITEM_COUNT (GAME_COUNT + 1) // +1 for "返回"

static lv_obj_t *s_scr;
static lv_obj_t *s_cards[MENU_ITEM_COUNT];
static lv_obj_t *s_rows[MENU_ITEM_COUNT];
static lv_obj_t *s_status[MENU_ITEM_COUNT];
static lv_obj_t *s_indicators[MENU_ITEM_COUNT];
static int s_sel;
static int s_active_game = -1;
static void (*s_on_exit)(void);

void demo_games_set_on_exit(void (*on_exit)(void))
{
    s_on_exit = on_exit;
}

static void games_menu_refresh(void)
{
    for (size_t i = 0; i < MENU_ITEM_COUNT; i++) {
        ui_system_set_item_state(s_cards[i], s_rows[i], s_status[i],
                                 s_indicators[i], (int)i == s_sel, true);
    }
}

static void games_menu_build(void)
{
    s_scr = ui_system_screen_create();

    lv_obj_t *back = ui_system_label(s_scr, "<", &lv_font_montserrat_20, UI_SYSTEM_TEXT);
    lv_obj_set_pos(back, 18, 42);

    lv_obj_t *heading = ui_system_label(s_scr, "游戏中心", &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 42);
    ui_system_divider(s_scr, 16, 77, 208);

    // 5 个游戏 + 返回: 行高 36 恰好满屏不滚动
    for (size_t i = 0; i < GAME_COUNT; i++) {
        int y = 92 + (int)i * 36;
        s_cards[i] = ui_system_item_create(s_scr, 16, y, 208, 32);
        s_rows[i] = ui_system_label(s_cards[i], GAMES[i].name,
                                    &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
        lv_obj_set_pos(s_rows[i], 16, 8);
        s_status[i] = ui_system_label(s_cards[i], GAMES[i].desc, &ui_font_noto_sc_14,
                                      UI_SYSTEM_MUTED);
        lv_obj_set_width(s_status[i], 60);
        lv_obj_set_style_text_align(s_status[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_status[i], 104, 8);
        s_indicators[i] = ui_system_label(s_cards[i], ">", &lv_font_montserrat_20,
                                           UI_SYSTEM_MUTED);
        lv_obj_set_pos(s_indicators[i], 180, 5);
    }

    // 返回选项
    int back_y = 92 + (int)GAME_COUNT * 36;
    s_cards[GAME_COUNT] = ui_system_item_create(s_scr, 16, back_y, 208, 32);
    s_rows[GAME_COUNT] = ui_system_label(s_cards[GAME_COUNT], "返回",
                                         &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
    lv_obj_set_pos(s_rows[GAME_COUNT], 16, 8);
    s_status[GAME_COUNT] = ui_system_label(s_cards[GAME_COUNT], "", &ui_font_noto_sc_14,
                                           UI_SYSTEM_MUTED);
    lv_obj_set_width(s_status[GAME_COUNT], 60);
    lv_obj_set_style_text_align(s_status[GAME_COUNT], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_status[GAME_COUNT], 104, 8);
    s_indicators[GAME_COUNT] = ui_system_label(s_cards[GAME_COUNT], ">", &lv_font_montserrat_20,
                                                UI_SYSTEM_MUTED);
    lv_obj_set_pos(s_indicators[GAME_COUNT], 180, 5);

    games_menu_refresh();
    lv_screen_load(s_scr);
    ui_status_set_visible(true);
}

void demo_games_enter(void)
{
    game_audio_init();
    s_active_game = -1;
    s_sel = 0;
    games_menu_build();
}

void demo_games_exit(void)
{
    if (s_active_game >= 0) {
        GAMES[s_active_game].exit();
        s_active_game = -1;
    }
    ui_status_set_visible(false);
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    for (size_t i = 0; i < MENU_ITEM_COUNT; i++) {
        s_cards[i] = NULL;
        s_rows[i] = NULL;
        s_status[i] = NULL;
        s_indicators[i] = NULL;
    }
    game_audio_deinit();
}

bool demo_games_back(void)
{
    if (s_active_game >= 0) {
        GAMES[s_active_game].exit();
        s_active_game = -1;
        games_menu_build();
        return true;
    }
    return false;
}

void demo_games_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (s_active_game >= 0) {
        GAMES[s_active_game].key(btn, ev);
        return;
    }

    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        game_audio_play(GAME_SFX_MOVE);
        s_sel = (s_sel + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
        games_menu_refresh();
        return;
    }

    if (btn == BSP_BTN_DOWN) {
        game_audio_play(GAME_SFX_MOVE);
        s_sel = (s_sel + 1) % MENU_ITEM_COUNT;
        games_menu_refresh();
        return;
    }

    if (btn == BSP_BTN_OK) {
        if (s_sel < (int)GAME_COUNT) {
            game_audio_play(GAME_SFX_POP);
            s_active_game = s_sel;
            ui_status_set_visible(false);
            if (s_scr) {
                lv_obj_delete(s_scr);
                s_scr = NULL;
            }
            for (size_t i = 0; i < MENU_ITEM_COUNT; i++) {
                s_cards[i] = NULL;
                s_rows[i] = NULL;
                s_status[i] = NULL;
                s_indicators[i] = NULL;
            }
            GAMES[s_active_game].enter();
        } else if (s_sel == (int)GAME_COUNT) {
            // 返回主菜单
            demo_games_exit();
            if (s_on_exit) {
                s_on_exit();
            }
        }
    }
}
