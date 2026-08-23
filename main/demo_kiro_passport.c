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
static lv_obj_t *s_request;
static lv_obj_t *s_hint;
static lv_timer_t *s_refresh_timer;

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

static void refresh_page(lv_timer_t *timer)
{
    (void)timer;
    if (!s_screen) return;

    kiro_passport_snapshot_t snapshot;
    kiro_passport_get_snapshot(&snapshot);
    lv_label_set_text(s_connection, connection_text(&snapshot));
    lv_label_set_text(s_state, state_text(&snapshot));

    if (snapshot.pending) {
        char request[128];
        snprintf(request, sizeof(request), "Tool: %s\n%s", snapshot.tool, snapshot.summary);
        lv_label_set_text(s_request, request);
        lv_label_set_text(s_hint, "OK: Allow    DOWN: Deny");
        if (s_mascot) ui_pixel_mascot_jump(s_mascot);
    } else if (snapshot.decision[0]) {
        lv_label_set_text(s_request,
                          strcmp(snapshot.decision, "allow") == 0 ? "Approved on Passport" : "Denied on Passport");
        lv_label_set_text(s_hint, "Long OK: Back to menu");
    } else {
        lv_label_set_text(s_request, "High-risk Kiro tools appear here.");
        lv_label_set_text(s_hint, "Long OK: Back to menu");
    }
}

void demo_kiro_passport_enter(void)
{
    s_screen = ui_system_screen_create();

    lv_obj_t *heading = ui_system_label(s_screen, "Kiro Passport", &ui_font_noto_sc_20,
                                        UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 38);
    ui_system_divider(s_screen, 16, 68, 208);

    s_mascot = ui_pixel_mascot_create(s_screen, 101, 80);
    s_connection = ui_system_label(s_screen, "BLE starting", &ui_font_noto_sc_14,
                                   UI_SYSTEM_MUTED);
    lv_obj_set_width(s_connection, 208);
    lv_obj_set_style_text_align(s_connection, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_connection, 16, 135);

    s_state = ui_system_label(s_screen, "Waiting for Kiro", &ui_font_noto_sc_14,
                              UI_SYSTEM_TEXT);
    lv_obj_set_width(s_state, 208);
    lv_obj_set_style_text_align(s_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_state, 16, 162);

    s_request = ui_system_label(s_screen, "High-risk Kiro tools appear here.",
                                &ui_font_noto_sc_14, UI_SYSTEM_TEXT);
    lv_obj_set_width(s_request, 196);
    lv_label_set_long_mode(s_request, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_request, 22, 196);

    s_hint = ui_system_label(s_screen, "Long OK: Back to menu", &ui_font_noto_sc_14,
                             UI_SYSTEM_MUTED);
    lv_obj_set_width(s_hint, 208);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_hint, 16, 283);

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
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_mascot = NULL;
    s_connection = NULL;
    s_state = NULL;
    s_request = NULL;
    s_hint = NULL;
}

void demo_kiro_passport_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event != BSP_BTN_CLICK) return;

    kiro_passport_snapshot_t snapshot;
    kiro_passport_get_snapshot(&snapshot);
    if (!snapshot.pending) return;

    if (btn == BSP_BTN_OK) {
        ESP_LOGI(TAG, "批准 Kiro 请求 %s", snapshot.request_id);
        kiro_passport_decide(true);
    } else if (btn == BSP_BTN_DOWN) {
        ESP_LOGI(TAG, "拒绝 Kiro 请求 %s", snapshot.request_id);
        kiro_passport_decide(false);
    }
    refresh_page(NULL);
}
