#include "demo.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "kiro_passport.h"
#include "kiro_passport_network.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "ui_pixel.h"
#include "ui_system.h"

static const char *TAG = "kiro_page";

static lv_obj_t *s_screen;
static lv_obj_t *s_mascot;
static lv_obj_t *s_connection;
static lv_obj_t *s_state;
static lv_obj_t *s_request_title;
static lv_obj_t *s_request_cont;
static lv_obj_t *s_request;
static lv_obj_t *s_hint;
static lv_timer_t *s_refresh_timer;

/* 是否允许 UP/DOWN 翻页。 */
static bool s_paged_content = false;

/* 记录当前已渲染的正文与标题哈希，避免 300ms 刷新打断用户滚动位置。
 * 使用 32 位哈希彻底支持任意长度长文本（防止 256 字节截断导致每次刷新都重置滚动位置）。 */
static uint32_t s_displayed_body_hash = 0;
static uint32_t s_displayed_title_hash = 0;

static uint32_t str_hash(const char *s)
{
    if (!s || !s[0]) return 0;
    uint32_t hash = 5381;
    while (*s) {
        hash = ((hash << 5) + hash) + (uint8_t)(*s++);
    }
    return hash ? hash : 1;
}

/* 最近一次已显示过的通知版本；用于判断是否有更新的通知需要展示。 */
static uint32_t s_displayed_notify_version = 0;

/* 审批模式：当前已处理过的请求 id，用于“边沿触发”吉祥物跳跃。
 * 只有在新请求进入时跳一次；若每次刷新都重跳，阶梯式向上漂移会把
 * 吉祥物顶出屏幕（表现为一直向上跳动后消失）。 */
static char s_jumped_request_id[KIRO_PASSPORT_REQUEST_ID_MAX] = {0};

static const char *connection_text(const kiro_passport_snapshot_t *snapshot)
{
    if (!snapshot->initialized) return "Passport unavailable";
    return kiro_passport_network_state_name(kiro_passport_network_get_state());
}

static const char *state_text(const kiro_passport_snapshot_t *snapshot)
{
    if (snapshot->pending) return "APPROVAL REQUIRED";
    if (strcmp(snapshot->state, "busy") == 0) return "Kiro is working";
    if (strcmp(snapshot->state, "idle") == 0) return "Kiro is ready";
    if (strcmp(snapshot->state, "error") == 0) return "Bridge error";
    return "Waiting for Kiro";
}

/* 展示“请求/正文”区域：放置在 s_request_cont 滚动容器中，支持 UP/DOWN 滚动。 */
static void set_request_paged(const char *title, const char *body, uint32_t title_color)
{
    const char *safe_title = title ? title : "";
    const char *safe_body = body ? body : "";

    lv_obj_set_style_text_color(s_request_title, lv_color_hex(title_color), 0);
    uint32_t title_hash = str_hash(safe_title);
    if (s_displayed_title_hash != title_hash) {
        s_displayed_title_hash = title_hash;
        lv_label_set_text(s_request_title, safe_title);
    }

    if (safe_body[0]) {
        lv_obj_clear_flag(s_request_cont, LV_OBJ_FLAG_HIDDEN);
        uint32_t body_hash = str_hash(safe_body);
        if (s_displayed_body_hash != body_hash) {
            s_displayed_body_hash = body_hash;
            lv_label_set_text(s_request, safe_body);
            lv_obj_set_height(s_request, LV_SIZE_CONTENT);
            lv_obj_update_layout(s_request);
            lv_obj_update_layout(s_request_cont);
            lv_obj_scroll_to_y(s_request_cont, 0, LV_ANIM_OFF);
        }
    } else {
        s_displayed_body_hash = 0;
        lv_obj_add_flag(s_request_cont, LV_OBJ_FLAG_HIDDEN);
    }
}

static void page_scroll(int32_t delta_y)
{
    if (s_request_cont) {
        lv_obj_scroll_by_bounded(s_request_cont, 0, delta_y, LV_ANIM_ON);
    }
}

/* 待审批模式下给吉祥物做一次性的“提醒”跳跃（边沿触发）。 */
static void mascot_nudge_pending(const kiro_passport_snapshot_t *snapshot)
{
    if (s_mascot && strcmp(snapshot->request_id, s_jumped_request_id) != 0) {
        snprintf(s_jumped_request_id, sizeof(s_jumped_request_id), "%s", snapshot->request_id);
        ui_pixel_mascot_jump_once(s_mascot);
    }
}

static void refresh_page(lv_timer_t *timer)
{
    (void)timer;
    if (!s_screen) return;

    kiro_passport_snapshot_t snapshot;
    kiro_passport_get_snapshot(&snapshot);
    kiro_passport_notify_info_t notify;

    if (snapshot.pending) {
        lv_label_set_text(s_connection, connection_text(&snapshot));
        /* 审批模式：展示待审批请求的工具与摘要，结合吉祥物跳跃。 */
        ui_pixel_mascot_stop_bounce(s_mascot);
        lv_label_set_text(s_state, "APPROVAL REQUIRED");
        char request[256];
        snprintf(request, sizeof(request), "%s\n%s", snapshot.tool, snapshot.summary);
        set_request_paged("REQUEST", request, UI_SYSTEM_ACCENT);
        lv_label_set_text(s_hint, "OK: Allow    DOWN: Deny");
        s_paged_content = false; /* 审批交互不吃翻页键，避免和 Allow/Deny 冲突 */
        mascot_nudge_pending(&snapshot);
    } else if (kiro_passport_network_get_notify(&notify) && notify.present) {
        /* 有通知时：隐藏 Relay ready，避免打扰通知展示，保持界面清爽。 */
        lv_label_set_text(s_connection, "");
        s_displayed_notify_version = notify.version;
        lv_label_set_text(s_state, notify.title);
        set_request_paged(NULL, notify.content, UI_SYSTEM_TEXT);
        s_paged_content = true;
        if (!notify.read) {
            /* 未读新消息推送提醒：小机器人持续跳动，单行按键提示 */
            lv_label_set_text(s_hint, "OK: Read   UP/DN: Page");
            ui_pixel_mascot_start_bounce(s_mascot);
        } else {
            /* 已按 OK 确认已读：小机器人停跳落地，内容保留，单行按键提示 */
            lv_label_set_text(s_hint, "Hold OK: Back   UP/DN: Page");
            ui_pixel_mascot_stop_bounce(s_mascot);
        }
    } else if (snapshot.decision[0]) {
        lv_label_set_text(s_connection, connection_text(&snapshot));
        ui_pixel_mascot_stop_bounce(s_mascot);
        lv_label_set_text(s_state, state_text(&snapshot));
        set_request_paged("DECISION",
                          strcmp(snapshot.decision, "allow") == 0 ? "Approved on Passport"
                                                                  : "Denied on Passport",
                          UI_SYSTEM_ACCENT);
        lv_label_set_text(s_hint, "Hold OK: Back to menu");
        s_paged_content = true;
    } else {
        lv_label_set_text(s_connection, connection_text(&snapshot));
        ui_pixel_mascot_stop_bounce(s_mascot);
        lv_label_set_text(s_state, state_text(&snapshot));
        set_request_paged("KIRO GUARD",
                          "Monitors high-risk AI tool calls.\n"
                          "Tool requests arrive here for approval.",
                          UI_SYSTEM_ACCENT);
        lv_label_set_text(s_hint, "Hold OK: Back to menu");
        s_paged_content = true;
    }
}

void demo_kiro_passport_enter(void)
{
    s_displayed_body_hash = 0;
    s_displayed_title_hash = 0;

    s_screen = ui_system_screen_create();

    lv_obj_t *heading = ui_system_label(s_screen, "Kiro Passport", &ui_font_noto_sc_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 38);
    ui_system_divider(s_screen, 16, 68, 208);

    /* 吉祥物固定在上部；连接与运行状态各占一行，中间用更紧凑的间距腾出正文空间。 */
    s_mascot = ui_pixel_mascot_create(s_screen, 101, 64);
    s_connection = ui_system_label(s_screen, "Relay starting", &ui_font_noto_sc_14,
                                   UI_SYSTEM_MUTED);
    lv_obj_set_width(s_connection, 208);
    lv_obj_set_style_text_align(s_connection, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_connection, 16, 118);

    s_state = ui_system_label(s_screen, "Waiting for Kiro", &ui_font_noto_sc_14,
                              UI_SYSTEM_TEXT);
    lv_obj_set_width(s_state, 208);
    lv_obj_set_style_text_align(s_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_state, 16, 140);

    ui_system_divider(s_screen, 16, 164, 208);

    /* 区段标题：REQUEST / DECISION / 通知标题。 */
    s_request_title = ui_system_label(s_screen, "", &ui_font_noto_sc_14,
                                      UI_SYSTEM_ACCENT);
    lv_obj_set_width(s_request_title, 196);
    lv_label_set_long_mode(s_request_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_request_title, 22, 172);

    /* 正文滚动容器：固定高度，超出部分支持 UP/DOWN 按键平滑滚动。 */
    s_request_cont = lv_obj_create(s_screen);
    lv_obj_set_pos(s_request_cont, 22, 188);
    lv_obj_set_size(s_request_cont, 196, 92);
    lv_obj_set_style_bg_opa(s_request_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_request_cont, 0, 0);
    lv_obj_set_style_pad_all(s_request_cont, 0, 0);
    lv_obj_add_flag(s_request_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_request_cont, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(s_request_cont, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(s_request_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_request_cont, LV_SCROLLBAR_MODE_AUTO);

    s_request = ui_system_label(s_request_cont, "", &ui_font_noto_sc_14,
                                UI_SYSTEM_TEXT);
    lv_obj_set_width(s_request, 196);
    lv_label_set_long_mode(s_request, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_request, 4, 0);
    lv_obj_set_pos(s_request, 0, 0);

    s_hint = ui_system_label(s_screen, "Hold OK: Back to menu", &ui_font_noto_sc_14,
                             UI_SYSTEM_MUTED);
    lv_obj_set_width(s_hint, 240);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_hint, 0, 284);

    s_refresh_timer = lv_timer_create(refresh_page, 300, NULL);
    refresh_page(NULL);
    lv_screen_load(s_screen);
}

void demo_kiro_passport_exit(void)
{
    if (s_refresh_timer) {
        lv_timer_delete(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    ui_pixel_mascot_stop_bounce(s_mascot);
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_mascot = NULL;
    s_connection = NULL;
    s_state = NULL;
    s_request_title = NULL;
    s_request_cont = NULL;
    s_request = NULL;
    s_hint = NULL;
    s_paged_content = false;
    s_displayed_body_hash = 0;
    s_displayed_title_hash = 0;
}

void demo_kiro_passport_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event != BSP_BTN_CLICK && event != BSP_BTN_HOLD) return;

    static kiro_passport_snapshot_t snapshot;
    kiro_passport_get_snapshot(&snapshot);

    /* 无待审批请求时，OK 键用于已读通知并平稳停跳，保留通知内容显示。仅响应单次点击。 */
    if (!snapshot.pending && btn == BSP_BTN_OK && event == BSP_BTN_CLICK) {
        if (kiro_passport_network_has_unread_notify()) {
            ESP_LOGI(TAG, "已读通知，机器人停跳落地，保持内容显示");
            kiro_passport_network_mark_notify_read();
            ui_pixel_mascot_stop_bounce(s_mascot);
            if (s_refresh_timer) lv_timer_ready(s_refresh_timer);
            return;
        }
    }

    /* 长正文（如通知）支持 UP/DOWN 上下翻页：
     * - 单击：步进 72 像素（4 行，占容器 ~80% 高度，保留 1 行视觉衔接上下文），大幅减少按键次数；
     * - 长按：支持 BSP_BTN_HOLD 连发平滑快翻（每次 54 像素 / 3 行），按住即可快速滑读长消息。 */
    if (s_paged_content) {
        int32_t step = (event == BSP_BTN_HOLD) ? 54 : 72;
        if (btn == BSP_BTN_UP) {
            page_scroll(step); /* 上一页/向上滚动 */
            return;
        } else if (btn == BSP_BTN_DOWN) {
            page_scroll(-step); /* 下一页/向下滚动 */
            return;
        }
    }

    if (!snapshot.pending || event != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_OK) {
        ESP_LOGI(TAG, "批准 Kiro 请求 %s", snapshot.request_id);
        kiro_passport_decide(true);
    } else if (btn == BSP_BTN_DOWN) {
        ESP_LOGI(TAG, "拒绝 Kiro 请求 %s", snapshot.request_id);
        kiro_passport_decide(false);
    }
    refresh_page(NULL);
}
