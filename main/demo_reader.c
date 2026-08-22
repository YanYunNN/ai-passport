// main/demo_reader.c —— 内置书库与按页阅读演示。
#include "demo.h"
#include "ui_pixel.h"
#include "lvgl.h"

#define ARRAY_SIZE(items) (sizeof(items) / sizeof((items)[0]))

typedef struct {
    const char *title;
    const char * const *pages;
    uint8_t page_count;
} builtin_book_t;

typedef enum {
    READER_LIBRARY,
    READER_READING,
} reader_view_t;

/*
 * 这些短篇直接编译到 Flash，避免 Demo 阶段引入文件系统和动态分配。
 * 当前工程只启用了 Montserrat 字体，故示例使用 ASCII 文本以保证真机可见。
 */
static const char * const LITTLE_CIRCUIT[] = {
    "At dusk, Mina found a\ntiny circuit under a\nbench. One blue light\nblinked beside a note:\nMAKE ONE KIND THING\nTODAY.",
    "Mina fixed the loose\nwire and built a lamp\nfor the dark hallway.\nEach neighbor added one\nsmall part: a switch,\na shade, a bright idea.",
    "When the lamp glowed,\nthe note changed. It\nread: KINDNESS IS A\nCIRCUIT. It GROWS\nWHEN WE PASS IT ON.",
    "Mina kept the circuit\nin her pocket. Whenever\nshe felt unsure, she\nlooked for one small\nthing she could make\nbrighter.",
};

static const char * const SKY_LIBRARY[] = {
    "Above the quiet town\nwas a library made of\nclouds. Each shelf held\na story that could only\nbe opened by curiosity.",
    "Arlo climbed the hill\nand asked the first\ncloud for a book. It\nhanded him a blank page\nand said: WRITE WHAT\nYOU WANT TO LEARN.",
    "Arlo wrote about stars,\nseeds, and distant seas.\nThe blank page filled\nwith maps, questions,\nand paths for tomorrow.",
    "Before he left, Arlo\nreturned the book. The\ncloud smiled: EVERY\nQUESTION MAKES ROOM\nFOR ANOTHER STORY.",
};

static const builtin_book_t BOOKS[] = {
    { "LITTLE CIRCUIT", LITTLE_CIRCUIT, ARRAY_SIZE(LITTLE_CIRCUIT) },
    { "SKY LIBRARY",    SKY_LIBRARY,    ARRAY_SIZE(SKY_LIBRARY) },
};
#define BOOK_COUNT ARRAY_SIZE(BOOKS)

static lv_obj_t *s_scr;
static lv_obj_t *s_content;
static lv_obj_t *s_cards[BOOK_COUNT];
static lv_obj_t *s_progress[BOOK_COUNT];
static lv_obj_t *s_text;
static lv_obj_t *s_page_info;
static uint8_t s_selected;
static uint8_t s_book_pages[BOOK_COUNT];
static reader_view_t s_view;

static void clear_content(void)
{
    if (s_content) {
        lv_obj_delete(s_content);
        s_content = NULL;
    }
    for (size_t i = 0; i < BOOK_COUNT; i++) {
        s_cards[i] = NULL;
        s_progress[i] = NULL;
    }
    s_text = NULL;
    s_page_info = NULL;
}

static lv_obj_t *content_create(void)
{
    lv_obj_t *content = lv_obj_create(s_scr);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(content, 0, 0);
    lv_obj_set_size(content, 240, 286);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    return content;
}

static void library_refresh(void)
{
    for (size_t i = 0; i < BOOK_COUNT; i++) {
        ui_pixel_set_selected(s_cards[i], i == s_selected, true);
        lv_label_set_text_fmt(s_progress[i], "PAGE %u/%u",
                              (unsigned)(s_book_pages[i] + 1),
                              (unsigned)BOOKS[i].page_count);
    }
}

static void library_build(void)
{
    clear_content();
    s_content = content_create();

    lv_obj_t *heading = ui_pixel_label(s_content, "2 BUILT-IN BOOKS",
                                       &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(heading, 48, 48);

    for (size_t i = 0; i < BOOK_COUNT; i++) {
        s_cards[i] = ui_pixel_panel_create(s_content, 17, 72 + (int)i * 63,
                                           206, 54, UI_PAPER);
        lv_obj_t *title = ui_pixel_label(s_cards[i], BOOKS[i].title,
                                         &lv_font_montserrat_14, UI_INK);
        lv_obj_set_pos(title, 10, 7);
        s_progress[i] = ui_pixel_label(s_cards[i], "", &lv_font_montserrat_14,
                                       UI_INK);
        lv_obj_set_pos(s_progress[i], 10, 28);
    }

    lv_obj_t *help = ui_pixel_label(s_content, "UP/DOWN: SELECT\nOK: READ",
                                    &lv_font_montserrat_14, UI_INK);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(help, 180);
    lv_obj_set_pos(help, 30, 204);
    ui_pixel_mascot_create(s_content, 101, 238);
    library_refresh();
}

static void reader_refresh(void)
{
    const builtin_book_t *book = &BOOKS[s_selected];
    uint8_t page = s_book_pages[s_selected];

    lv_label_set_text_static(s_text, book->pages[page]);
    lv_label_set_text_fmt(s_page_info, "%s  %u/%u", book->title,
                          (unsigned)(page + 1), (unsigned)book->page_count);
}

static void reader_build(void)
{
    clear_content();
    s_content = content_create();

    lv_obj_t *panel = ui_pixel_panel_create(s_content, 10, 54, 220, 205, UI_PAPER);
    s_text = lv_label_create(panel);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_text, 194);
    lv_obj_set_style_text_font(s_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(UI_INK), 0);
    lv_obj_set_pos(s_text, 9, 11);

    s_page_info = lv_label_create(panel);
    lv_obj_set_style_text_font(s_page_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_page_info, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_set_pos(s_page_info, 9, 159);

    lv_obj_t *help = ui_pixel_label(s_content, "UP: PREV  DOWN: NEXT\nOK: LIBRARY",
                                    &lv_font_montserrat_14, UI_INK);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(help, 220);
    lv_obj_set_pos(help, 10, 264);
    reader_refresh();
}

void demo_reader_enter(void)
{
    s_selected = 0;
    s_view = READER_LIBRARY;
    s_scr = ui_pixel_screen_create("READER");
    library_build();
    lv_screen_load(s_scr);
}

void demo_reader_exit(void)
{
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_content = NULL;
    s_text = NULL;
    s_page_info = NULL;
    for (size_t i = 0; i < BOOK_COUNT; i++) {
        s_cards[i] = NULL;
        s_progress[i] = NULL;
    }
}

void demo_reader_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;

    if (s_view == READER_LIBRARY) {
        if (btn == BSP_BTN_UP) {
            s_selected = (s_selected + BOOK_COUNT - 1) % BOOK_COUNT;
            library_refresh();
        } else if (btn == BSP_BTN_DOWN) {
            s_selected = (s_selected + 1) % BOOK_COUNT;
            library_refresh();
        } else if (btn == BSP_BTN_OK) {
            s_view = READER_READING;
            reader_build();
        }
        return;
    }

    if (btn == BSP_BTN_UP) {
        if (s_book_pages[s_selected] > 0) s_book_pages[s_selected]--;
        reader_refresh();
    } else if (btn == BSP_BTN_DOWN) {
        if (s_book_pages[s_selected] + 1 < BOOKS[s_selected].page_count) {
            s_book_pages[s_selected]++;
        }
        reader_refresh();
    } else if (btn == BSP_BTN_OK) {
        s_view = READER_LIBRARY;
        library_build();
    }
}
