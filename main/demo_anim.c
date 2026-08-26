// main/demo_anim.c - 动态帧动画 / 慢速视频流演示（内置 2 套素材与播放控制）
#include "demo.h"
#include "ui_system.h"
#include "ui_status.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "bsp_button.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "demo_anim";

#define FRAME_INTERVAL_MS 66     // ~15 FPS 慢速流/帧动画
#define STAR_COUNT        22
#define SPECTRUM_BARS     8
#define SPARK_COUNT       6

typedef enum {
    MATERIAL_CYBER_RADAR = 0,
    MATERIAL_PIXEL_MECH  = 1,
    MATERIAL_COUNT       = 2,
} anim_material_t;

static const char *MATERIAL_NAMES[] = {
    "1. 赛博星际雷达",
    "2. 像素机甲流",
};

// UI 根节点与全局控件
static lv_obj_t *s_scr;
static lv_timer_t *s_play_timer;
static anim_material_t s_material = MATERIAL_CYBER_RADAR;
static bool s_is_playing = true;
static uint32_t s_frame_idx = 0;

// 顶部栏
static lv_obj_t *s_title_label;
static lv_obj_t *s_badge_label;
static lv_obj_t *s_fps_label;

// 主视口容器
static lv_obj_t *s_viewport;

// 底部控制与信息栏
static lv_obj_t *s_info_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_progress_bar;
static lv_obj_t *s_hint_label;

// ---------------------------------------------------------------------------
// 素材 1：赛博星际与雷达控件
// ---------------------------------------------------------------------------
typedef struct {
    float x, y, z;
    lv_obj_t *dot;
} star_t;

static star_t s_stars[STAR_COUNT];
static lv_obj_t *s_radar_center;
static lv_obj_t *s_radar_ring1;
static lv_obj_t *s_radar_ring2;
static lv_obj_t *s_radar_sweep_line;
static lv_point_precise_t s_sweep_pts[2];
static lv_obj_t *s_planet;
static lv_obj_t *s_planet_ring;
static lv_obj_t *s_telemetry_label;
static lv_obj_t *s_blip1;
static lv_obj_t *s_blip2;
static lv_obj_t *s_wave_line;
static lv_point_precise_t s_wave_pts[16];

// ---------------------------------------------------------------------------
// 素材 2：像素机甲与复古奔跑流控件
// ---------------------------------------------------------------------------
static lv_obj_t *s_mech_root;
static lv_obj_t *s_mech_body;
static lv_obj_t *s_mech_head;
static lv_obj_t *s_mech_antenna;
static lv_obj_t *s_mech_eye_l;
static lv_obj_t *s_mech_eye_r;
static lv_obj_t *s_mech_leg_l;
static lv_obj_t *s_mech_leg_r;
static lv_obj_t *s_mech_arm_l;
static lv_obj_t *s_mech_arm_r;
static lv_obj_t *s_sparks[SPARK_COUNT];
static lv_obj_t *s_ground_blocks[8];
static lv_obj_t *s_city_buildings[5];
static lv_obj_t *s_spectrum_bars[SPECTRUM_BARS];
static lv_obj_t *s_coin;
static lv_obj_t *s_coin_shadow;

// ---------------------------------------------------------------------------
// 辅助函数：快速创建像素色块
// ---------------------------------------------------------------------------
static lv_obj_t *create_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

// ---------------------------------------------------------------------------
// 素材 1 构建与帧更新
// ---------------------------------------------------------------------------
static void cyber_radar_build(void)
{
    // 星空粒子
    for (int i = 0; i < STAR_COUNT; i++) {
        s_stars[i].x = (float)((rand() % 200) - 100);
        s_stars[i].y = (float)((rand() % 160) - 80);
        s_stars[i].z = (float)((rand() % 100) + 10);
        s_stars[i].dot = create_box(s_viewport, 120, 95, 2, 2, 0x4A80B0, 0);
    }

    // 行星与光环
    s_planet = create_box(s_viewport, 30, 25, 34, 34, 0x7E3FF2, 17);
    lv_obj_set_style_border_color(s_planet, lv_color_hex(0xB68CFF), 0);
    lv_obj_set_style_border_width(s_planet, 2, 0);

    s_planet_ring = lv_obj_create(s_viewport);
    lv_obj_remove_flag(s_planet_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_planet_ring, 20, 36);
    lv_obj_set_size(s_planet_ring, 54, 12);
    lv_obj_set_style_radius(s_planet_ring, 6, 0);
    lv_obj_set_style_bg_opa(s_planet_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_planet_ring, lv_color_hex(0x00FFCC), 0);
    lv_obj_set_style_border_width(s_planet_ring, 2, 0);
    lv_obj_set_style_pad_all(s_planet_ring, 0, 0);

    // 雷达圆环与中心
    s_radar_ring1 = lv_obj_create(s_viewport);
    lv_obj_remove_flag(s_radar_ring1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_radar_ring1, 60, 35);
    lv_obj_set_size(s_radar_ring1, 120, 120);
    lv_obj_set_style_radius(s_radar_ring1, 60, 0);
    lv_obj_set_style_bg_opa(s_radar_ring1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_radar_ring1, lv_color_hex(0x1B4E6B), 0);
    lv_obj_set_style_border_width(s_radar_ring1, 1, 0);
    lv_obj_set_style_pad_all(s_radar_ring1, 0, 0);

    s_radar_ring2 = lv_obj_create(s_viewport);
    lv_obj_remove_flag(s_radar_ring2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_radar_ring2, 85, 60);
    lv_obj_set_size(s_radar_ring2, 70, 70);
    lv_obj_set_style_radius(s_radar_ring2, 35, 0);
    lv_obj_set_style_bg_opa(s_radar_ring2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_radar_ring2, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(s_radar_ring2, 1, 0);
    lv_obj_set_style_pad_all(s_radar_ring2, 0, 0);

    s_radar_center = create_box(s_viewport, 118, 93, 4, 4, 0x00FFCC, 2);

    // 扫描信标点
    s_blip1 = create_box(s_viewport, 145, 70, 5, 5, 0xFF3366, 2);
    s_blip2 = create_box(s_viewport, 90, 120, 4, 4, 0xFFD700, 2);

    // 扫描线
    s_sweep_pts[0].x = 120;
    s_sweep_pts[0].y = 95;
    s_sweep_pts[1].x = 180;
    s_sweep_pts[1].y = 95;
    s_radar_sweep_line = lv_line_create(s_viewport);
    lv_line_set_points(s_radar_sweep_line, s_sweep_pts, 2);
    lv_obj_set_style_line_color(s_radar_sweep_line, lv_color_hex(0x00FFCC), 0);
    lv_obj_set_style_line_width(s_radar_sweep_line, 2, 0);

    // 底部波形发生线
    for (int i = 0; i < 16; i++) {
        s_wave_pts[i].x = 10 + i * 14;
        s_wave_pts[i].y = 175;
    }
    s_wave_line = lv_line_create(s_viewport);
    lv_line_set_points(s_wave_line, s_wave_pts, 16);
    lv_obj_set_style_line_color(s_wave_line, lv_color_hex(0x39FF14), 0);
    lv_obj_set_style_line_width(s_wave_line, 2, 0);

    // 遥测 HUD
    s_telemetry_label = lv_label_create(s_viewport);
    lv_obj_set_style_text_font(s_telemetry_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_telemetry_label, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_pos(s_telemetry_label, 12, 140);
    lv_label_set_text(s_telemetry_label, "RADAR: ACTIVE | WARP 0.95c");
}

static void cyber_radar_update(uint32_t frame)
{
    // 1. 星空跃迁粒子更新
    for (int i = 0; i < STAR_COUNT; i++) {
        s_stars[i].z -= 2.2f;
        if (s_stars[i].z <= 2.0f) {
            s_stars[i].x = (float)((rand() % 200) - 100);
            s_stars[i].y = (float)((rand() % 160) - 80);
            s_stars[i].z = 100.0f;
        }
        float scale = 60.0f / s_stars[i].z;
        int sx = 120 + (int)(s_stars[i].x * scale);
        int sy = 95 + (int)(s_stars[i].y * scale);
        int size = (s_stars[i].z < 35.0f) ? 3 : (s_stars[i].z < 65.0f ? 2 : 1);
        uint32_t col = (s_stars[i].z < 35.0f) ? 0x00FFFF : (s_stars[i].z < 65.0f ? 0x88CCFF : 0x336699);

        if (sx >= 0 && sx < 236 && sy >= 0 && sy < 190) {
            lv_obj_set_pos(s_stars[i].dot, sx, sy);
            lv_obj_set_size(s_stars[i].dot, size, size);
            lv_obj_set_style_bg_color(s_stars[i].dot, lv_color_hex(col), 0);
            lv_obj_clear_flag(s_stars[i].dot, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_stars[i].dot, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 2. 雷达旋转扫描线
    float angle = (float)(frame % 60) * (3.1415926f * 2.0f / 60.0f);
    int ex = 120 + (int)(cosf(angle) * 58.0f);
    int ey = 95 + (int)(sinf(angle) * 58.0f);
    s_sweep_pts[1].x = ex;
    s_sweep_pts[1].y = ey;
    lv_line_set_points(s_radar_sweep_line, s_sweep_pts, 2);

    // 3. 信标点闪烁
    if ((frame / 6) % 2 == 0) {
        lv_obj_clear_flag(s_blip1, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_blip1, LV_OBJ_FLAG_HIDDEN);
    }
    if ((frame / 4) % 3 != 0) {
        lv_obj_clear_flag(s_blip2, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_blip2, LV_OBJ_FLAG_HIDDEN);
    }

    // 4. 波动能量线更新
    for (int i = 0; i < 16; i++) {
        float wave_phase = (float)frame * 0.25f + (float)i * 0.6f;
        int dy = (int)(sinf(wave_phase) * 6.0f);
        s_wave_pts[i].y = 180 + dy;
    }
    lv_line_set_points(s_wave_line, s_wave_pts, 16);

    // 5. 遥测文本
    char buf[64];
    int heading = (int)((frame * 6) % 360);
    snprintf(buf, sizeof(buf), "HDG: %03d° | T1: %2.1fK | PWR 98%%",
             heading, 3.2f + (sinf((float)frame * 0.1f) * 1.5f));
    lv_label_set_text(s_telemetry_label, buf);
}

// ---------------------------------------------------------------------------
// 素材 2 构建与帧更新
// ---------------------------------------------------------------------------
static void pixel_mech_build(void)
{
    // 背景远景天际线大楼
    int b_x[5] = { 10, 55, 110, 160, 200 };
    int b_w[5] = { 35, 45, 40, 32, 30 };
    int b_h[5] = { 60, 85, 70, 90, 55 };
    for (int i = 0; i < 5; i++) {
        s_city_buildings[i] = create_box(s_viewport, b_x[i], 160 - b_h[i], b_w[i], b_h[i], 0x182030, 2);
        lv_obj_set_style_border_color(s_city_buildings[i], lv_color_hex(0x283B54), 0);
        lv_obj_set_style_border_width(s_city_buildings[i], 1, 0);
    }

    // 滚动地面方块
    for (int i = 0; i < 8; i++) {
        s_ground_blocks[i] = create_box(s_viewport, i * 32, 155, 30, 12, 0x00E5FF, 2);
        lv_obj_set_style_border_color(s_ground_blocks[i], lv_color_hex(0x005577), 0);
        lv_obj_set_style_border_width(s_ground_blocks[i], 1, 0);
    }

    // 旋转能量币
    s_coin_shadow = create_box(s_viewport, 195, 122, 14, 4, 0x0A101D, 2);
    s_coin = create_box(s_viewport, 195, 105, 14, 14, 0xFFD700, 7);
    lv_obj_set_style_border_color(s_coin, lv_color_hex(0xFF9900), 0);
    lv_obj_set_style_border_width(s_coin, 2, 0);

    // 机甲根节点容器
    s_mech_root = lv_obj_create(s_viewport);
    lv_obj_remove_flag(s_mech_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_mech_root, 65, 95);
    lv_obj_set_size(s_mech_root, 60, 65);
    lv_obj_set_style_bg_opa(s_mech_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_mech_root, 0, 0);
    lv_obj_set_style_pad_all(s_mech_root, 0, 0);

    // 喷射火花粒子
    for (int i = 0; i < SPARK_COUNT; i++) {
        s_sparks[i] = create_box(s_mech_root, 8, 38, 4, 4, 0xFF5500, 1);
    }

    // 机甲手臂（后）
    s_mech_arm_l = create_box(s_mech_root, 36, 22, 7, 14, 0x4B3B8A, 3);

    // 腿部（后/前）
    s_mech_leg_l = create_box(s_mech_root, 18, 38, 7, 18, 0x2A3547, 2);
    s_mech_leg_r = create_box(s_mech_root, 28, 38, 7, 18, 0x3F516B, 2);

    // 机甲身体与头部
    s_mech_body = create_box(s_mech_root, 14, 20, 24, 20, 0x6C5CE7, 4);
    lv_obj_set_style_border_color(s_mech_body, lv_color_hex(0xA29BFE), 0);
    lv_obj_set_style_border_width(s_mech_body, 1, 0);

    s_mech_head = create_box(s_mech_root, 16, 6, 20, 15, 0x00CEC9, 3);
    s_mech_antenna = create_box(s_mech_root, 24, 0, 4, 7, 0xFF7675, 1);

    // 眼睛
    s_mech_eye_l = create_box(s_mech_root, 19, 10, 4, 5, 0x1B1464, 1);
    s_mech_eye_r = create_box(s_mech_root, 27, 10, 4, 5, 0x1B1464, 1);

    // 机甲手臂（前）
    s_mech_arm_r = create_box(s_mech_root, 10, 22, 7, 14, 0x81ECEC, 3);

    // 底部 8 段频谱柱
    for (int i = 0; i < SPECTRUM_BARS; i++) {
        s_spectrum_bars[i] = create_box(s_viewport, 18 + i * 26, 186, 18, 8, 0x00FF88, 2);
    }
}

static void pixel_mech_update(uint32_t frame)
{
    // 1. 地面平滑横移
    int offset = (int)((frame * 6) % 32);
    for (int i = 0; i < 8; i++) {
        int gx = i * 32 - offset;
        lv_obj_set_x(s_ground_blocks[i], gx);
    }

    // 2. 远景大楼极慢视差横移
    int city_offset = (int)((frame * 2) % 60);
    int base_x[5] = { 10, 55, 110, 160, 200 };
    for (int i = 0; i < 5; i++) {
        int cx = base_x[i] - city_offset;
        if (cx < -40) cx += 260;
        lv_obj_set_x(s_city_buildings[i], cx);
    }

    // 3. 奔跑步态骨骼帧
    int step_phase = (frame % 8);
    int bob_y = (step_phase == 1 || step_phase == 5) ? -2 : (step_phase == 3 || step_phase == 7 ? 2 : 0);
    lv_obj_set_y(s_mech_body, 20 + bob_y);
    lv_obj_set_y(s_mech_head, 6 + bob_y);
    lv_obj_set_y(s_mech_antenna, 0 + bob_y);
    lv_obj_set_y(s_mech_eye_l, 10 + bob_y);
    lv_obj_set_y(s_mech_eye_r, 10 + bob_y);

    // 天线灯随节奏变色
    uint32_t ant_col = (frame % 4 < 2) ? 0xFF0055 : 0x00FFFF;
    lv_obj_set_style_bg_color(s_mech_antenna, lv_color_hex(ant_col), 0);

    // 腿部交替动作
    int leg_l_y = 38, leg_r_y = 38;
    int leg_l_h = 18, leg_r_h = 18;
    if (step_phase < 4) {
        leg_l_y = 35;
        leg_l_h = 14;
        leg_r_y = 39;
        leg_r_h = 19;
    } else {
        leg_l_y = 39;
        leg_l_h = 19;
        leg_r_y = 35;
        leg_r_h = 14;
    }
    lv_obj_set_pos(s_mech_leg_l, 18, leg_l_y);
    lv_obj_set_size(s_mech_leg_l, 7, leg_l_h);
    lv_obj_set_pos(s_mech_leg_r, 28, leg_r_y);
    lv_obj_set_size(s_mech_leg_r, 7, leg_r_h);

    // 手臂交替摆动
    int arm_l_y = (step_phase < 4) ? 24 : 19;
    int arm_r_y = (step_phase < 4) ? 19 : 24;
    lv_obj_set_y(s_mech_arm_l, arm_l_y + bob_y);
    lv_obj_set_y(s_mech_arm_r, arm_r_y + bob_y);

    // 4. 喷射尾焰火花
    for (int i = 0; i < SPARK_COUNT; i++) {
        int sp_phase = (frame + i * 3) % 10;
        int sx = 8 - sp_phase * 3;
        int sy = 34 + (i % 3) * 3 + (sp_phase % 2);
        lv_obj_set_pos(s_sparks[i], sx, sy);
        uint32_t sp_col = (sp_phase < 4) ? 0xFFEE00 : (sp_phase < 7 ? 0xFF4400 : 0x661100);
        lv_obj_set_style_bg_color(s_sparks[i], lv_color_hex(sp_col), 0);
    }

    // 5. 金币旋转与横移
    int coin_x = 220 - (int)((frame * 5) % 240);
    int coin_w = 4 + abs((int)(sinf((float)frame * 0.4f) * 10.0f));
    lv_obj_set_pos(s_coin, coin_x, 105 + (int)(sinf((float)frame * 0.2f) * 4.0f));
    lv_obj_set_size(s_coin, coin_w, 14);
    lv_obj_set_pos(s_coin_shadow, coin_x, 153);
    lv_obj_set_size(s_coin_shadow, coin_w, 3);

    // 6. 音频流频谱跳动
    for (int i = 0; i < SPECTRUM_BARS; i++) {
        int height = 4 + abs((int)(sinf((float)frame * 0.4f + (float)i * 1.1f) * 16.0f));
        lv_obj_set_pos(s_spectrum_bars[i], 18 + i * 26, 192 - height);
        lv_obj_set_size(s_spectrum_bars[i], 18, height);
        uint32_t spec_col = (height > 14) ? 0xFF3366 : (height > 9 ? 0xFFD700 : 0x00FF88);
        lv_obj_set_style_bg_color(s_spectrum_bars[i], lv_color_hex(spec_col), 0);
    }
}

// ---------------------------------------------------------------------------
// 切换与重绘素材视口
// ---------------------------------------------------------------------------
static void load_current_material(void)
{
    if (!s_viewport) return;

    // 清理子视口所有对象
    lv_obj_clean(s_viewport);

    // 根据当前素材重新构建
    if (s_material == MATERIAL_CYBER_RADAR) {
        cyber_radar_build();
    } else {
        pixel_mech_build();
    }

    if (s_title_label) {
        lv_label_set_text(s_title_label, MATERIAL_NAMES[s_material]);
    }
}

// ---------------------------------------------------------------------------
// 帧时钟驱动
// ---------------------------------------------------------------------------
static void on_frame_timer(lv_timer_t *timer)
{
    (void)timer;
    if (!s_is_playing) return;

    s_frame_idx++;

    if (s_material == MATERIAL_CYBER_RADAR) {
        cyber_radar_update(s_frame_idx);
    } else {
        pixel_mech_update(s_frame_idx);
    }

    // 更新时间码与进度
    uint32_t total_sec = (s_frame_idx * FRAME_INTERVAL_MS) / 1000;
    uint32_t ms_part = ((s_frame_idx * FRAME_INTERVAL_MS) % 1000) / 100;
    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%02lu:%02lu.%lu | #%04lu",
             (unsigned long)(total_sec / 60), (unsigned long)(total_sec % 60),
             (unsigned long)ms_part, (unsigned long)(s_frame_idx % 10000));
    if (s_time_label) {
        lv_label_set_text(s_time_label, time_str);
    }

    // 进度条（每 300 帧循环一圈）
    int progress = (int)((s_frame_idx % 300) * 100 / 300);
    if (s_progress_bar) {
        lv_bar_set_value(s_progress_bar, progress, LV_ANIM_OFF);
    }
}

static void update_play_status_ui(void)
{
    if (s_badge_label) {
        if (s_is_playing) {
            lv_label_set_text(s_badge_label, "[PLAY]");
            lv_obj_set_style_text_color(s_badge_label, lv_color_hex(0x00FF88), 0);
        } else {
            lv_label_set_text(s_badge_label, "[PAUSE]");
            lv_obj_set_style_text_color(s_badge_label, lv_color_hex(0xFF9900), 0);
        }
    }
}

// ---------------------------------------------------------------------------
// 页面生命周期
// ---------------------------------------------------------------------------
void demo_anim_enter(void)
{
    ESP_LOGI(TAG, "进入动画与慢速视频流页面");
    s_scr = ui_system_screen_create();
    s_frame_idx = 0;
    s_is_playing = true;

    // 1. 顶部标题栏
    lv_obj_t *header = create_box(s_scr, 0, 0, 240, 36, UI_SYSTEM_SURFACE, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(UI_SYSTEM_BORDER), 0);
    lv_obj_set_style_border_width(header, 1, 0);

    s_title_label = lv_label_create(header);
    lv_obj_set_style_text_font(s_title_label, &ui_font_noto_sc_14, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(UI_SYSTEM_TEXT), 0);
    lv_obj_set_pos(s_title_label, 10, 8);
    lv_label_set_text(s_title_label, MATERIAL_NAMES[s_material]);

    s_badge_label = lv_label_create(header);
    lv_obj_set_style_text_font(s_badge_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_badge_label, 138, 8);

    s_fps_label = lv_label_create(header);
    lv_obj_set_style_text_font(s_fps_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_fps_label, lv_color_hex(UI_SYSTEM_MUTED), 0);
    lv_obj_set_pos(s_fps_label, 194, 8);
    lv_label_set_text(s_fps_label, "15F");

    update_play_status_ui();

    // 2. 视频流核心视口区（240x196）
    s_viewport = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_viewport, 0, 36);
    lv_obj_set_size(s_viewport, 240, 196);
    lv_obj_set_style_radius(s_viewport, 0, 0);
    lv_obj_set_style_border_width(s_viewport, 0, 0);
    lv_obj_set_style_pad_all(s_viewport, 0, 0);
    lv_obj_set_style_bg_color(s_viewport, lv_color_hex(0x06080D), 0);

    // 3. 底部信息与控制栏
    lv_obj_t *footer = create_box(s_scr, 0, 232, 240, 88, UI_SYSTEM_SURFACE, 0);
    lv_obj_set_style_border_color(footer, lv_color_hex(UI_SYSTEM_BORDER), 0);
    lv_obj_set_style_border_width(footer, 1, 0);

    // 进度条
    s_progress_bar = lv_bar_create(footer);
    lv_obj_set_pos(s_progress_bar, 12, 8);
    lv_obj_set_size(s_progress_bar, 216, 4);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x222B38), 0);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);

    // 状态流信息
    s_info_label = lv_label_create(footer);
    lv_obj_set_style_text_font(s_info_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info_label, lv_color_hex(UI_SYSTEM_ACCENT), 0);
    lv_obj_set_pos(s_info_label, 12, 18);
    lv_label_set_text(s_info_label, "STREAM RAW");

    s_time_label = lv_label_create(footer);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(UI_SYSTEM_TEXT), 0);
    lv_obj_set_pos(s_time_label, 110, 18);
    lv_label_set_text(s_time_label, "00:00.0 | #0000");

    // 操作指引
    s_hint_label = lv_label_create(footer);
    lv_obj_set_style_text_font(s_hint_label, &ui_font_noto_sc_14, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(UI_SYSTEM_MUTED), 0);
    lv_obj_set_pos(s_hint_label, 12, 42);
    lv_label_set_text(s_hint_label, "▲/▼: 切换素材\nOK: 播放/暂停 (长按返回)");

    load_current_material();

    // 启动帧更新定时器
    s_play_timer = lv_timer_create(on_frame_timer, FRAME_INTERVAL_MS, NULL);

    lv_screen_load(s_scr);
    ui_status_set_visible(false);
}

void demo_anim_exit(void)
{
    ESP_LOGI(TAG, "退出动画与慢速视频流页面");
    if (s_play_timer) {
        lv_timer_delete(s_play_timer);
        s_play_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_viewport = NULL;
        s_title_label = NULL;
        s_badge_label = NULL;
        s_fps_label = NULL;
        s_info_label = NULL;
        s_time_label = NULL;
        s_progress_bar = NULL;
        s_hint_label = NULL;
    }
}

void demo_anim_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_OK) {
        // 播放 / 暂停切换
        s_is_playing = !s_is_playing;
        update_play_status_ui();
    } else if (btn == BSP_BTN_UP) {
        // 上一个素材
        s_material = (s_material + MATERIAL_COUNT - 1) % MATERIAL_COUNT;
        s_frame_idx = 0;
        load_current_material();
    } else if (btn == BSP_BTN_DOWN) {
        // 下一个素材
        s_material = (s_material + 1) % MATERIAL_COUNT;
        s_frame_idx = 0;
        load_current_material();
    }
}
