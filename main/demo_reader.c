// main/demo_reader.c - 内置书库与按页阅读演示。
#include "demo.h"
#include "ui_font_noto_sc_14.h"
#include "ui_status.h"
#include "lvgl.h"

#define ARRAY_SIZE(items) (sizeof(items) / sizeof((items)[0]))

/* Reader 专属深色主题。避免修改共享像素主题，以免影响其他硬件 Demo。 */
#define READER_BG              0x0B0D11
#define READER_SURFACE         0x191D24
#define READER_SURFACE_ACTIVE  0xECE9DF
#define READER_BORDER          0x303641
#define READER_TEXT            0xF3F1EB
#define READER_MUTED           0xA6ABB5
#define READER_TEXT_ACTIVE     0x13161B
#define READER_ACCENT          0xC6AA70
#define READER_TRACK           0x363B45
#define READER_PAGE_MAX        4

typedef struct {
    const char *title;
    const char * const *pages;
    uint8_t page_count;
    const lv_font_t *font;
} builtin_book_t;

typedef enum {
    READER_LIBRARY,
    READER_READING,
} reader_view_t;

/*
 * 所有书籍直接编译到 Flash，避免 Demo 阶段引入文件系统和动态分配。
 * 英文短篇使用 Montserrat；中文短篇使用受限 Noto Sans SC 字符子集。
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

/* 原创中文修真短篇，不包含任何第三方小说正文。 */
static const char * const QINGLAN_PATH[] = {
    "青岚村外，夜雨初歇。\n少年陆川在石桥下\n拾到一枚温热的青玉。\n玉面只刻着两字：守心。",
    "次日清晨，陆川沿着\n山路上行。雾里传来\n钟声，他看见古观门前\n有一盏未灭的灯。",
    "老人递来木剑，说：\n修行先学取舍。若只求\n快，心便会比脚步更早\n迷路。",
    "陆川将青玉放回掌中，\n朝晨光走去。他知道前方\n没有捷径，只有每日不改\n的问心。",
};

static const builtin_book_t BOOKS[] = {
    { "LITTLE CIRCUIT", LITTLE_CIRCUIT, ARRAY_SIZE(LITTLE_CIRCUIT), &lv_font_montserrat_14 },
    { "SKY LIBRARY",    SKY_LIBRARY,    ARRAY_SIZE(SKY_LIBRARY),    &lv_font_montserrat_14 },
    { "青岚问道",        QINGLAN_PATH,   ARRAY_SIZE(QINGLAN_PATH),   &ui_font_noto_sc_14 },
};
#define BOOK_COUNT ARRAY_SIZE(BOOKS)

static lv_obj_t *s_scr;
static lv_obj_t *s_content;
static lv_obj_t *s_cards[BOOK_COUNT];
static lv_obj_t *s_card_titles[BOOK_COUNT];
static lv_obj_t *s_card_markers[BOOK_COUNT];
static lv_obj_t *s_progress[BOOK_COUNT];
static lv_obj_t *s_text;
static lv_obj_t *s_page_info;
static lv_obj_t *s_page_steps[READER_PAGE_MAX];
static uint8_t s_selected;
static uint8_t s_book_pages[BOOK_COUNT];
static reader_view_t s_view;

static lv_obj_t *reader_block(lv_obj_t *parent, int x, int y, int w, int h,
                              uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *reader_surface(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color, uint32_t border)
{
    lv_obj_t *surface = reader_block(parent, x, y, w, h, color);
    lv_obj_set_style_radius(surface, 10, 0);
    lv_obj_set_style_border_width(surface, 1, 0);
    lv_obj_set_style_border_color(surface, lv_color_hex(border), 0);
    return surface;
}

static lv_obj_t *reader_label(lv_obj_t *parent, const char *text,
                              const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *reader_screen_create(void)
{
    lv_obj_t *screen = reader_block(NULL, 0, 0, 240, 320, READER_BG);
    lv_obj_set_style_radius(screen, 0, 0);
    return screen;
}

static void clear_content(void)
{
    if (s_content) {
        lv_obj_delete(s_content);
        s_content = NULL;
    }
    for (size_t i = 0; i < BOOK_COUNT; i++) {
        s_cards[i] = NULL;
        s_card_titles[i] = NULL;
        s_card_markers[i] = NULL;
        s_progress[i] = NULL;
    }
    for (size_t i = 0; i < READER_PAGE_MAX; i++) {
        s_page_steps[i] = NULL;
    }
    s_text = NULL;
    s_page_info = NULL;
}

static lv_obj_t *content_create(void)
{
    lv_obj_t *content = reader_block(s_scr, 0, 0, 240, 320, READER_BG);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    return content;
}

static void add_header(lv_obj_t *parent, const char *section, const char *detail)
{
    lv_obj_t *section_label = reader_label(parent, section, &lv_font_montserrat_14,
                                           READER_TEXT);
    lv_obj_set_pos(section_label, 16, 37);

    lv_obj_t *detail_label = reader_label(parent, detail, &lv_font_montserrat_14,
                                          READER_MUTED);
    lv_obj_set_width(detail_label, 92);
    lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(detail_label, 132, 37);

    reader_block(parent, 16, 59, 208, 1, READER_BORDER);
}

static void library_refresh(void)
{
    for (size_t i = 0; i < BOOK_COUNT; i++) {
        bool selected = i == s_selected;
        uint32_t card_color = selected ? READER_SURFACE_ACTIVE : READER_SURFACE;
        uint32_t text_color = selected ? READER_TEXT_ACTIVE : READER_TEXT;
        uint32_t muted_color = selected ? 0x50555D : READER_MUTED;

        lv_obj_set_style_bg_color(s_cards[i], lv_color_hex(card_color), 0);
        lv_obj_set_style_border_color(s_cards[i],
                                      lv_color_hex(selected ? READER_ACCENT : READER_BORDER), 0);
        lv_obj_set_style_border_width(s_cards[i], selected ? 2 : 1, 0);
        lv_obj_set_style_text_color(s_card_titles[i], lv_color_hex(text_color), 0);
        lv_obj_set_style_text_color(s_card_markers[i], lv_color_hex(muted_color), 0);
        lv_obj_set_style_text_color(s_progress[i], lv_color_hex(muted_color), 0);
        lv_label_set_text_fmt(s_progress[i], "PAGE %u / %u",
                              (unsigned)(s_book_pages[i] + 1),
                              (unsigned)BOOKS[i].page_count);
    }
}

static void library_build(void)
{
    clear_content();
    s_content = content_create();
    add_header(s_content, "LVGL", "3 BOOKS");

    lv_obj_t *back = reader_label(s_content, "<", &lv_font_montserrat_20,
                                  READER_TEXT);
    lv_obj_set_pos(back, 18, 70);

    lv_obj_t *heading = reader_label(s_content, "YOUR SHELF", &lv_font_montserrat_20,
                                     READER_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 72);

    for (size_t i = 0; i < BOOK_COUNT; i++) {
        int y = 104 + (int)i * 66;
        s_cards[i] = reader_surface(s_content, 16, y, 208, 60,
                                    READER_SURFACE, READER_BORDER);
        reader_block(s_cards[i], 0, 12, 4, 36, READER_ACCENT);

        s_card_titles[i] = reader_label(s_cards[i], BOOKS[i].title,
                                        BOOKS[i].font, READER_TEXT);
        lv_obj_set_pos(s_card_titles[i], 17, 12);

        s_progress[i] = reader_label(s_cards[i], "", &lv_font_montserrat_14,
                                     READER_MUTED);
        lv_obj_set_pos(s_progress[i], 17, 36);

        s_card_markers[i] = reader_label(s_cards[i], ">", &lv_font_montserrat_20,
                                         READER_MUTED);
        lv_obj_set_pos(s_card_markers[i], 180, 17);
    }

    library_refresh();
}

static void reader_refresh(void)
{
    const builtin_book_t *book = &BOOKS[s_selected];
    uint8_t page = s_book_pages[s_selected];

    lv_label_set_text_static(s_text, book->pages[page]);
    lv_label_set_text_fmt(s_page_info, "PAGE %u / %u",
                          (unsigned)(page + 1), (unsigned)book->page_count);

    for (size_t i = 0; i < READER_PAGE_MAX; i++) {
        if (i < book->page_count) {
            lv_obj_remove_flag(s_page_steps[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_page_steps[i],
                                      lv_color_hex(i <= page ? READER_ACCENT : READER_TRACK), 0);
        } else {
            lv_obj_add_flag(s_page_steps[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void reader_build(void)
{
    clear_content();
    s_content = content_create();

    const builtin_book_t *book = &BOOKS[s_selected];
    add_header(s_content, "READING", "");

    lv_obj_t *title = reader_label(s_content, book->title, book->font, READER_TEXT);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(title, 208);
    lv_obj_set_pos(title, 16, 72);

    lv_obj_t *panel = reader_surface(s_content, 16, 96, 208, 166,
                                     READER_SURFACE, READER_BORDER);
    s_text = lv_label_create(panel);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_text, 180);
    lv_obj_set_style_text_font(s_text, book->font, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(READER_TEXT), 0);
    lv_obj_set_style_text_line_space(s_text, 4, 0);
    lv_obj_set_pos(s_text, 14, 14);

    s_page_info = reader_label(s_content, "", &lv_font_montserrat_14, READER_MUTED);
    lv_obj_set_width(s_page_info, 208);
    lv_obj_set_style_text_align(s_page_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_page_info, 16, 273);

    for (size_t i = 0; i < READER_PAGE_MAX; i++) {
        s_page_steps[i] = reader_block(s_content, 16 + (int)i * 53, 300, 49, 4,
                                       READER_TRACK);
        lv_obj_set_style_radius(s_page_steps[i], 2, 0);
    }

    reader_refresh();
}

void demo_reader_enter(void)
{
    s_selected = 0;
    s_view = READER_LIBRARY;
    s_scr = reader_screen_create();
    library_build();
    lv_screen_load(s_scr);
    ui_status_set_visible(true);
}

void demo_reader_exit(void)
{
    ui_status_set_visible(false);
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_content = NULL;
    s_text = NULL;
    s_page_info = NULL;
    for (size_t i = 0; i < BOOK_COUNT; i++) {
        s_cards[i] = NULL;
        s_card_titles[i] = NULL;
        s_card_markers[i] = NULL;
        s_progress[i] = NULL;
    }
    for (size_t i = 0; i < READER_PAGE_MAX; i++) {
        s_page_steps[i] = NULL;
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
