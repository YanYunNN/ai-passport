// main/demo_chat.c —— Chat 语音助手：流式录音 → ASR → AI 对话 → TTS → 播放。
// 吸取小智 AI（xiaozhi-esp32）架构精髓：
// 1. 全程复用常驻的 WebSocket 连接（kiro_passport_network），不重复建立 TLS（省 35KB 堆内存）。
// 2. 录音 PCM 边采边发（流式），零 Flash 擦写阻塞。
// 3. 服务端 ASR/LLM 回复后下发 MP3 流，软解播放，端到端延迟显著降低。
#include "demo.h"
#include "app_settings.h"
#include "power_manager.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "kiro_passport_network.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "ui_system.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mp3dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "demo_chat";

#define CHAT_SAMPLE_RATE 16000
#define CHAT_REC_MAX_SEC 4
#define CHAT_REC_MAX_BYTES (128 * 1024) // 131072 字节 (4KB 对齐, ~4.1s)
#define CHAT_MP3_MAX_BYTES (160 * 1024)
#define CHAT_MP3_OFFSET 131072 // 对齐到 4KB
#define CHAT_STORE_NAME "chatrec"
#define CHAT_STORE_SUBTYPE 0x40
#define CHAT_AI_TEXT_MAX 512
#define CHAT_HISTORY_MAX 6
#define CHAT_MP3_READBUF_MAX 4096

typedef enum {
    CHAT_IDLE,
    CHAT_RECORDING,
    CHAT_WAITING,
    CHAT_PLAYING,
} chat_state_t;

typedef struct {
    char role[16];
    char content[CHAT_AI_TEXT_MAX];
} chat_message_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_status;
static lv_obj_t *s_log;
static TaskHandle_t s_task;
static volatile chat_state_t s_state = CHAT_IDLE;
static volatile bool s_stop_record;
static const esp_partition_t *s_store;
static size_t s_rec_len;
static size_t s_mp3_len;
static chat_message_t s_history[CHAT_HISTORY_MAX];
static size_t s_history_count;
static char s_transcript[CHAT_AI_TEXT_MAX * 2 + 32];
static char s_recognized[CHAT_AI_TEXT_MAX];
static char s_reply[CHAT_AI_TEXT_MAX];
static char s_voice_err[128];
static volatile bool s_tts_ready;
static volatile bool s_tts_error;
static HMP3Decoder s_mp3;
static lv_obj_t *s_marquee_bar;
static lv_obj_t *s_marquee_label;

static void chat_log_heap(const char *stage)
{
    ESP_LOGI(TAG, "heap@%s: free=%lu min=%lu", stage,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());
}

static void chat_set_status(const char *text)
{
    if (!bsp_lvgl_lock(500)) return;
    if (s_status) lv_label_set_text(s_status, text);
    bsp_lvgl_unlock();
}

static void chat_show_transcript(void)
{
    if (!bsp_lvgl_lock(500)) return;
    if (s_log) lv_label_set_text(s_log, s_transcript);
    bsp_lvgl_unlock();
}

static void chat_reset_to_idle(const char *status)
{
    s_state = CHAT_IDLE;
    s_stop_record = false;
    if (status) chat_set_status(status);
}

static esp_err_t chat_store_init(void)
{
    if (s_store) return ESP_OK;
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, CHAT_STORE_SUBTYPE, CHAT_STORE_NAME);
    if (!part) return ESP_ERR_NOT_FOUND;
    if (part->size < CHAT_MP3_OFFSET + CHAT_MP3_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    s_store = part;
    ESP_LOGI(TAG, "chatrec 分区: offset=0x%lx size=0x%lx", (unsigned long)part->address,
             (unsigned long)part->size);
    return ESP_OK;
}

static esp_err_t chat_store_erase(size_t offset, size_t size)
{
    if (!s_store) return ESP_ERR_INVALID_STATE;
    size_t aligned_offset = offset & ~4095;
    size_t aligned_size = (size + 4095) & ~4095;
    if (aligned_offset + aligned_size > s_store->size) {
        aligned_size = s_store->size - aligned_offset;
    }
    esp_err_t err = esp_partition_erase_range(s_store, aligned_offset, aligned_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chat_store_erase 失败: off=0x%zx sz=0x%zx err=%s",
                 aligned_offset, aligned_size, esp_err_to_name(err));
    }
    return err;
}

static size_t chat_json_escape(char *out, size_t out_size, const char *text)
{
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && pos + 6 < out_size; p++) {
        char c = (char)*p;
        switch (c) {
        case '"':  out[pos++] = '\\'; out[pos++] = '"'; break;
        case '\\': out[pos++] = '\\'; out[pos++] = '\\'; break;
        case '\n': out[pos++] = '\\'; out[pos++] = 'n'; break;
        case '\r': out[pos++] = '\\'; out[pos++] = 'r'; break;
        case '\t': out[pos++] = '\\'; out[pos++] = 't'; break;
        default:
            if (c < 0x20) {
                out[pos++] = ' ';
            } else {
                out[pos++] = c;
            }
            break;
        }
    }
    out[pos] = '\0';
    return pos;
}

static void chat_push_history(const char *role, const char *content)
{
    if (s_history_count >= CHAT_HISTORY_MAX) {
        memmove(&s_history[0], &s_history[1], (CHAT_HISTORY_MAX - 1) * sizeof(s_history[0]));
        s_history_count = CHAT_HISTORY_MAX - 1;
    }
    strlcpy(s_history[s_history_count].role, role, sizeof(s_history[s_history_count].role));
    strlcpy(s_history[s_history_count].content, content,
            sizeof(s_history[s_history_count].content));
    s_history_count++;
}

static void chat_marquee_stop_locked(void)
{
    if (s_marquee_label) {
        lv_anim_del(s_marquee_label, NULL);
        lv_obj_delete(s_marquee_label);
        s_marquee_label = NULL;
    }
}

static void chat_marquee_start(const char *text)
{
    if (!bsp_lvgl_lock(500)) return;
    chat_marquee_stop_locked();
    if (!s_marquee_bar) {
        s_marquee_bar = lv_obj_create(s_scr);
        lv_obj_set_pos(s_marquee_bar, 14, 250);
        lv_obj_set_size(s_marquee_bar, 212, 28);
        lv_obj_set_style_bg_color(s_marquee_bar, lv_color_hex(UI_SYSTEM_SURFACE), 0);
        lv_obj_set_style_border_color(s_marquee_bar, lv_color_hex(UI_SYSTEM_BORDER), 0);
        lv_obj_set_style_border_width(s_marquee_bar, 1, 0);
        lv_obj_set_style_radius(s_marquee_bar, 4, 0);
        lv_obj_set_style_pad_all(s_marquee_bar, 0, 0);
    }
    s_marquee_label = lv_label_create(s_marquee_bar);
    lv_label_set_text(s_marquee_label, text);
    lv_obj_set_style_text_font(s_marquee_label, &ui_font_noto_sc_14, 0);
    lv_obj_set_style_text_color(s_marquee_label, lv_color_hex(UI_SYSTEM_ACCENT), 0);
    lv_obj_set_width(s_marquee_label, LV_SIZE_CONTENT);
    lv_obj_update_layout(s_marquee_label);
    lv_coord_t text_width = lv_obj_get_width(s_marquee_label);
    const lv_coord_t start_x = 212;
    const lv_coord_t end_x = -text_width;
    lv_obj_align(s_marquee_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_x(s_marquee_label, start_x);
    uint32_t duration_ms = (uint32_t)((start_x - end_x) * 1000u / 40u); /* 40 px/s */
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_marquee_label);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&anim, start_x, end_x);
    lv_anim_set_duration(&anim, duration_ms);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_start(&anim);
    bsp_lvgl_unlock();
}

static void chat_marquee_stop(void)
{
    if (!bsp_lvgl_lock(500)) return;
    chat_marquee_stop_locked();
    bsp_lvgl_unlock();
}

static void chat_voice_cb(kiro_passport_voice_event_t event, const void *data, size_t len, void *user_ctx)
{
    (void)user_ctx;
    switch (event) {
    case KIRO_PASSPORT_VOICE_EVT_START_ACK:
        ESP_LOGI(TAG, "Voice session ack");
        break;
    case KIRO_PASSPORT_VOICE_EVT_ASR:
        if (data && len > 0) {
            strlcpy(s_recognized, (const char *)data, sizeof(s_recognized));
            ESP_LOGI(TAG, "ASR text: %s", s_recognized);
            snprintf(s_transcript, sizeof(s_transcript), "你: %s\nAI: …", s_recognized);
            chat_show_transcript();
            chat_push_history("user", s_recognized);
            chat_set_status("AI 思考中…");
        }
        break;
    case KIRO_PASSPORT_VOICE_EVT_REPLY:
        if (data && len > 0) {
            strlcpy(s_reply, (const char *)data, sizeof(s_reply));
            ESP_LOGI(TAG, "Reply text: %s", s_reply);
            snprintf(s_transcript, sizeof(s_transcript), "你: %s\nAI: %s", s_recognized, s_reply);
            chat_show_transcript();
            chat_push_history("assistant", s_reply);
            chat_set_status("接收语音中…");
        }
        break;
    case KIRO_PASSPORT_VOICE_EVT_TTS_START:
        ESP_LOGI(TAG, "TTS start: total_bytes=%zu", len);
        s_mp3_len = 0;
        break;
    case KIRO_PASSPORT_VOICE_EVT_TTS_DATA:
        if (data && len > 0 && s_store) {
            if (s_mp3_len + len <= CHAT_MP3_MAX_BYTES) {
                esp_partition_write(s_store, CHAT_MP3_OFFSET + s_mp3_len, data, len);
                s_mp3_len += len;
            }
        }
        break;
    case KIRO_PASSPORT_VOICE_EVT_TTS_END:
        ESP_LOGI(TAG, "TTS end: total received %zu bytes", s_mp3_len);
        s_tts_ready = true;
        break;
    case KIRO_PASSPORT_VOICE_EVT_ERROR:
        if (data && len > 0) {
            strlcpy(s_voice_err, (const char *)data, sizeof(s_voice_err));
            ESP_LOGE(TAG, "Voice error: %s", s_voice_err);
            s_tts_error = true;
        }
        break;
    }
}

static esp_err_t chat_play_mp3_stream(void)
{
    if (!s_store || s_mp3_len == 0) return ESP_ERR_INVALID_STATE;
    if (!s_mp3) {
        s_mp3 = MP3InitDecoder();
        if (!s_mp3) return ESP_ERR_NO_MEM;
    }
    bsp_audio_set_volume(app_settings_get_volume_percent());
    int16_t *pcm = malloc(sizeof(int16_t) * 1152 * 2);
    uint8_t *buf = malloc(CHAT_MP3_READBUF_MAX);
    if (!pcm || !buf) {
        if (pcm) free(pcm);
        if (buf) free(buf);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_OK;
    const size_t buf_cap = CHAT_MP3_READBUF_MAX;
    size_t buf_len = 0;
    size_t flash_off = 0;
    bool format_set = false;

    while (true) {
        if (s_stop_record) break;

        if (buf_len < buf_cap && flash_off < s_mp3_len) {
            size_t want = buf_cap - buf_len;
            if (want > s_mp3_len - flash_off) want = s_mp3_len - flash_off;
            if (esp_partition_read(s_store, CHAT_MP3_OFFSET + flash_off, buf + buf_len, want) != ESP_OK) {
                break;
            }
            flash_off += want;
            buf_len += want;
        }
        if (buf_len == 0) {
            if (s_tts_ready && flash_off >= s_mp3_len) break;
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        int offset = MP3FindSyncWord(buf, (int)buf_len);
        if (offset < 0) {
            if (flash_off < s_mp3_len || !s_tts_ready) {
                // 跳过 ID3/非音频元数据头，保留末尾 3 字节以防跨包同步字截断
                if (buf_len > 3) {
                    memmove(buf, buf + buf_len - 3, 3);
                    buf_len = 3;
                }
                if (flash_off >= s_mp3_len && !s_tts_ready) {
                    vTaskDelay(pdMS_TO_TICKS(15));
                }
                continue;
            }
            break;
        }
        unsigned char *in = buf + offset;
        int in_left = (int)(buf_len - offset);
        int before = in_left;
        int err = MP3Decode(s_mp3, &in, &in_left, pcm, 0);
        memmove(buf, in, (size_t)in_left);
        buf_len = (size_t)in_left;

        if (err == ERR_MP3_NONE) {
            MP3FrameInfo info;
            MP3GetLastFrameInfo(s_mp3, &info);
            if (!format_set) {
                uint8_t channels = info.nChans > 1 ? 2 : 1;
                if (bsp_audio_set_format((uint32_t)info.samprate, 16, channels) != ESP_OK) {
                    result = ESP_FAIL;
                    break;
                }
                format_set = true;
                // 软渐入前 160 个采样，消除解码器冷启动爆破杂音
                size_t ramp_samps = info.outputSamps > 160 ? 160 : info.outputSamps;
                for (size_t i = 0; i < ramp_samps; i++) {
                    pcm[i] = (int16_t)((int32_t)pcm[i] * i / ramp_samps);
                }
            }
            size_t pcm_bytes = (size_t)info.outputSamps * (info.nChans > 1 ? 2u : 1u) * sizeof(int16_t);
            if (bsp_audio_write(pcm, pcm_bytes) != ESP_OK) break;
        } else if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            if (flash_off >= s_mp3_len) {
                if (s_tts_ready) break;
                vTaskDelay(pdMS_TO_TICKS(15));
                continue;
            }
        } else if (in_left == before) {
            if (buf_len > 0) {
                memmove(buf, buf + 1, buf_len - 1);
                buf_len--;
            } else {
                if (s_tts_ready && flash_off >= s_mp3_len) break;
                vTaskDelay(pdMS_TO_TICKS(15));
            }
        }
    }
    free(pcm);
    free(buf);
    return result;
}

static void chat_task(void *arg)
{
    (void)arg;

    for (;;) {
        chat_state_t st = s_state;
        if (st == CHAT_IDLE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (st == CHAT_RECORDING) {
            if (!kiro_passport_network_is_connected()) {
                chat_reset_to_idle("Relay 未连接，请稍候");
                continue;
            }
            if (chat_store_init() != ESP_OK) {
                chat_reset_to_idle("存储分区缺失");
                continue;
            }

            s_tts_ready = false;
            s_tts_error = false;
            s_mp3_len = 0;
            s_voice_err[0] = '\0';
            s_recognized[0] = '\0';
            s_reply[0] = '\0';

            /* 1. 构建对话历史 JSON 并启动语音流会话 */
            static char s_hist_buf[1024];
            size_t hpos = 0;
            hpos += snprintf(s_hist_buf + hpos, sizeof(s_hist_buf) - hpos, "[");
            for (size_t i = 0; i < s_history_count; i++) {
                int n = snprintf(s_hist_buf + hpos, sizeof(s_hist_buf) - hpos,
                                 "%s{\"role\":\"%s\",\"content\":\"",
                                 i ? "," : "", s_history[i].role);
                if (n < 0 || hpos + n >= sizeof(s_hist_buf)) break;
                hpos += n;
                hpos += chat_json_escape(s_hist_buf + hpos, sizeof(s_hist_buf) - hpos, s_history[i].content);
                int tail = snprintf(s_hist_buf + hpos, sizeof(s_hist_buf) - hpos, "\"}");
                if (tail < 0 || hpos + tail >= sizeof(s_hist_buf)) break;
                hpos += tail;
            }
            snprintf(s_hist_buf + hpos, sizeof(s_hist_buf) - hpos, "]");

            esp_err_t start_err = kiro_passport_network_voice_start(s_hist_buf);
            if (start_err != ESP_OK) {
                chat_reset_to_idle("会话发起失败");
                continue;
            }

            /* 2. 边采边发（真正的 Prism 流式管线：零 Flash 擦写与零 Flash 存转阻塞） */
            chat_set_status("聆听中… (再按 OK 停止)");
            if (bsp_audio_set_format(CHAT_SAMPLE_RATE, 16, 1) != ESP_OK) {
                kiro_passport_network_voice_end();
                chat_reset_to_idle("音频初始化失败");
                continue;
            }
            bsp_audio_set_volume(app_settings_get_volume_percent());

            s_rec_len = 0;
            static int16_t s_pcm_chunk[512]; // 1024 字节 (32ms 采样)
            bool stream_ok = true;
            while (s_rec_len < CHAT_REC_MAX_BYTES) {
                if (s_stop_record) break;
                if (bsp_audio_read(s_pcm_chunk, sizeof(s_pcm_chunk)) != ESP_OK) {
                    stream_ok = false;
                    break;
                }
                if (kiro_passport_network_voice_send_pcm(s_pcm_chunk, sizeof(s_pcm_chunk)) != ESP_OK) {
                    ESP_LOGW(TAG, "PCM 流式推送失败");
                    stream_ok = false;
                    break;
                }
                s_rec_len += sizeof(s_pcm_chunk);
            }
            bsp_audio_close(); // 录音完毕立即释放 I2S DMA
            s_stop_record = false; // 必须清除停止录音标志，防止进入 WAITING 时被当成提前取消而立即触发超时！

            if (!stream_ok || s_rec_len < 1600 * sizeof(int16_t)) {
                kiro_passport_network_voice_end();
                chat_reset_to_idle(stream_ok ? "没听清，再试一次" : "网络发送中断");
                continue;
            }

            /* 3. 通知服务端语音结束，进入 AI 思考等待 */
            kiro_passport_network_voice_end();
            chat_set_status("AI 思考中…");
            s_stop_record = false;
            s_state = CHAT_WAITING;
            continue;
        }

        if (st == CHAT_WAITING) {
            uint32_t wait_ticks = 0;
            while (!s_tts_error && wait_ticks < 600) {
                if (s_state != CHAT_WAITING) {
                    break;
                }
                if (s_stop_record) {
                    // 用户在等待期间再次按 OK 主动取消
                    break;
                }
                // 收到 2048 字节 (约 0.25 秒音频) 或服务端已告知结束，立刻提前起播！
                if (s_mp3_len >= 2048 || s_tts_ready) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                wait_ticks++;
            }

            if (s_state != CHAT_WAITING) {
                continue;
            }
            if (s_stop_record) {
                chat_reset_to_idle("已取消");
                continue;
            }
            if (s_tts_error) {
                chat_reset_to_idle(s_voice_err[0] ? s_voice_err : "识别失败");
                continue;
            }
            if (s_mp3_len == 0 && !s_tts_ready) {
                ESP_LOGW(TAG, "等待 TTS 响应超时: wait_ticks=%lu", (unsigned long)wait_ticks);
                chat_reset_to_idle("响应超时");
                continue;
            }

            s_state = CHAT_PLAYING;
            continue;
        }

        if (st == CHAT_PLAYING) {
            chat_set_status("朗读中… (长按 OK 退出)");
            chat_log_heap("playing_start");
            chat_marquee_start(s_reply);
            chat_play_mp3_stream();
            chat_marquee_stop();
            bsp_audio_close();
            if (s_mp3) {
                MP3FreeDecoder(s_mp3);
                s_mp3 = NULL;
            }
            chat_log_heap("playing_end");
            /* 播放完毕在空闲期预擦除 MP3 分区，为下一次对话做准备 */
            chat_store_erase(CHAT_MP3_OFFSET, 128 * 1024);
            chat_reset_to_idle("OK: 录音  ·  长按 OK: 退出");
            continue;
        }
    }
}

void demo_chat_enter(void)
{
    /* 聊天交互期间暂时关闭浅睡眠，防止 Wi-Fi/DMA/网络发送在 CPU 降频或休眠时挂起 */
    power_manager_set_light_sleep_enabled(false);

    /* 注册 Voice 交互回调 */
    kiro_passport_network_register_voice_cb(chat_voice_cb, NULL);

    s_transcript[0] = '\0';
    s_recognized[0] = '\0';
    s_reply[0] = '\0';
    s_history_count = 0;
    s_stop_record = false;
    s_state = CHAT_IDLE;

    if (!s_task) {
        if (xTaskCreate(chat_task, "demo_chat", 4096, NULL, 4, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "创建 chat_task 失败");
            s_task = NULL;
        }
    }
    if (chat_store_init() == ESP_OK) {
        /* 进入 Chat 页面时预擦除 MP3 分区，避免交互过程中擦除 Flash 阻塞网络 */
        chat_store_erase(CHAT_MP3_OFFSET, 128 * 1024);
    }

    s_scr = ui_system_screen_create();
    lv_obj_t *heading = ui_system_label(s_scr, "Chat", &ui_font_noto_sc_20, UI_SYSTEM_TEXT);
    lv_obj_set_width(heading, 208);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(heading, 16, 28);
    ui_system_divider(s_scr, 16, 56, 208);

    s_status = ui_system_label(s_scr, "", &ui_font_noto_sc_14, UI_SYSTEM_MUTED);
    lv_obj_set_width(s_status, 208);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_status, 16, 68);

    lv_obj_t *panel = lv_obj_create(s_scr);
    lv_obj_set_pos(panel, 14, 100);
    lv_obj_set_size(panel, 212, 140);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_SYSTEM_SURFACE), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_SYSTEM_BORDER), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 6, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);

    s_log = lv_label_create(panel);
    lv_obj_set_width(s_log, 196);
    lv_label_set_long_mode(s_log, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_log, &ui_font_noto_sc_14, 0);
    lv_obj_set_style_text_color(s_log, lv_color_hex(UI_SYSTEM_TEXT), 0);

    lv_obj_t *hint = ui_system_label(s_scr, "OK: 录音  ·  长按 OK: 退出", &ui_font_noto_sc_14,
                                     UI_SYSTEM_MUTED);
    lv_obj_set_width(hint, 208);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 16, 292);

    kiro_passport_network_config_t cfg;
    kiro_passport_network_get_config(&cfg);
    if (!s_task) {
        lv_label_set_text(s_status, "内存不足，请重启");
    } else if (!cfg.credential[0]) {
        lv_label_set_text(s_status, "未配对：设置 → Relay 先配对");
    } else if (wifi_manager_get_state() != WIFI_MANAGER_CONNECTED) {
        lv_label_set_text(s_status, "等待 Wi-Fi 连接…");
    } else if (!kiro_passport_network_is_connected()) {
        lv_label_set_text(s_status, "等待 Relay 连接…");
    } else {
        lv_label_set_text(s_status, "按 OK 开始录音");
    }

    chat_log_heap("chat_enter");
    lv_screen_load(s_scr);
}

void demo_chat_exit(void)
{
    /* 注销 Voice 交互回调 */
    kiro_passport_network_register_voice_cb(NULL, NULL);

    s_state = CHAT_IDLE;
    s_stop_record = true;
    bsp_audio_close();
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_mp3) {
        MP3FreeDecoder(s_mp3);
        s_mp3 = NULL;
    }
    s_history_count = 0;
    if (s_marquee_bar) {
        lv_obj_delete(s_marquee_bar);
        s_marquee_bar = NULL;
    }
    s_marquee_label = NULL;
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_status = NULL;
        s_log = NULL;
    }
    chat_log_heap("chat_exit");
    power_manager_set_light_sleep_enabled(app_settings_get()->light_sleep_enabled);
}

void demo_chat_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    if (s_state == CHAT_IDLE && btn == BSP_BTN_OK) {
        kiro_passport_network_config_t cfg;
        kiro_passport_network_get_config(&cfg);
        if (!cfg.credential[0]) {
            chat_set_status("未配对：设置 → Relay");
            return;
        }
        if (wifi_manager_get_state() != WIFI_MANAGER_CONNECTED) {
            chat_set_status("Wi-Fi 未连接");
            return;
        }
        if (!kiro_passport_network_is_connected()) {
            chat_set_status("Relay 未连接");
            return;
        }
        s_stop_record = false;
        s_state = CHAT_RECORDING;
    } else if ((s_state == CHAT_RECORDING || s_state == CHAT_WAITING) && btn == BSP_BTN_OK) {
        s_stop_record = true;
    }
}

bool demo_chat_back(void)
{
    if (s_state != CHAT_IDLE) {
        chat_set_status("处理中，请稍候…");
        return true;
    }
    return false;
}
