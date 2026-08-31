// main/demo_chat.c —— Chat 语音助手：录音 → ASR → AI 对话 → TTS → 播放 全流程。
// 云端复用 relay 的 /v1/voice/asr|chat|tts（ws.yanyun.asia），鉴权用设备凭证
// （Authorization: Bearer 设备凭证 + X-Device-Id），与 Kiro WebSocket 同一套凭证。
#include "demo.h"
#include "app_settings.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "esp_crt_bundle.h"
#include "kiro_passport_network.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "ui_system.h"
#include "wifi_manager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mp3dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "demo_chat";

#define CHAT_SAMPLE_RATE 16000
/* 录音上限 5 秒 = 160KB；C3 无 PSRAM，分配失败自动降级 3 秒（96KB）。 */
#define CHAT_RECORD_MAX_SEC 5
#define CHAT_RECORD_BYTES (CHAT_SAMPLE_RATE * 2 * CHAT_RECORD_MAX_SEC)
#define CHAT_RECORD_FALLBACK_SEC 3
#define CHAT_RECORD_FALLBACK_BYTES (CHAT_SAMPLE_RATE * 2 * CHAT_RECORD_FALLBACK_SEC)
/* TTS 响应（base64 MP3 的 JSON）缓冲上限。 */
#define CHAT_TTS_BUF_SIZE (160 * 1024)
#define CHAT_HTTP_TIMEOUT_MS 20000
#define CHAT_AI_TEXT_MAX 512
#define CHAT_HISTORY_MAX 6
#define CHAT_BOUNDARY "KiroChatBoundary7f3a"

typedef enum {
    CHAT_IDLE,
    CHAT_RECORDING,
    CHAT_ASR,
    CHAT_CHATTING,
    CHAT_TTS,
    CHAT_PLAYING,
} chat_state_t;

typedef struct {
    char role[16];
    char content[CHAT_AI_TEXT_MAX];
} chat_message_t;

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
} chat_http_response_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_status;
static lv_obj_t *s_log;
static TaskHandle_t s_task;
static volatile chat_state_t s_state = CHAT_IDLE;
static volatile bool s_stop_record;
static int16_t *s_record_buf;
static size_t s_record_cap;
static size_t s_record_len;
static uint8_t *s_tts_buf;
static size_t s_mp3_len;
static chat_message_t s_history[CHAT_HISTORY_MAX];
static size_t s_history_count;
static char s_transcript[CHAT_AI_TEXT_MAX * 2 + 32];
static HMP3Decoder s_mp3;
/* 底部滚动字幕条：TTS 播放时滚动 AI 回复。 */
static lv_obj_t *s_marquee_bar;
static lv_obj_t *s_marquee_label;

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

static void chat_marquee_stop_locked(void)
{
    if (s_marquee_label) {
        lv_anim_del(s_marquee_label, NULL);
        lv_obj_delete(s_marquee_label);
        s_marquee_label = NULL;
    }
}

/* 启动底部字幕条：AI 回复从右向左循环滚动（40px/s），与 TTS 播放同步。 */
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
    /* LV_SIZE_CONTENT 自适应文本宽度，避免依赖 LVGL 内部文本测量 API。 */
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

static void chat_reset_to_idle(const char *status)
{
    s_state = CHAT_IDLE;
    s_stop_record = false;
    if (status) chat_set_status(status);
}

/* relay_url 形如 "wss://ws.yanyun.asia"，转成 HTTPS 前缀再拼 path。 */
static void chat_build_url(const char *relay_url, const char *path, char *out, size_t out_size)
{
    if (strncmp(relay_url, "wss://", 6) == 0) {
        snprintf(out, out_size, "https://%s%s", relay_url + 6, path);
    } else if (strncmp(relay_url, "ws://", 5) == 0) {
        snprintf(out, out_size, "http://%s%s", relay_url + 5, path);
    } else {
        snprintf(out, out_size, "%s%s", relay_url, path);
    }
}

static esp_err_t chat_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data || event->data_len <= 0) {
        return ESP_OK;
    }
    chat_http_response_t *response = event->user_data;
    if (response->length + (size_t)event->data_len >= response->capacity) {
        response->overflow = true;
        return ESP_FAIL;
    }
    memcpy(response->data + response->length, event->data, event->data_len);
    response->length += (size_t)event->data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

/* WAV 44 字节头（16k/16bit/mono），data 段大小 = pcm_bytes。 */
static void chat_build_wav_header(uint8_t *header, size_t pcm_bytes)
{
    memset(header, 0, 44);
    memcpy(header, "RIFF", 4);
    *(uint32_t *)(header + 4) = 36u + (uint32_t)pcm_bytes;
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    *(uint32_t *)(header + 16) = 16;
    *(uint16_t *)(header + 20) = 1; /* PCM */
    *(uint16_t *)(header + 22) = 1; /* mono */
    *(uint32_t *)(header + 24) = CHAT_SAMPLE_RATE;
    *(uint32_t *)(header + 28) = CHAT_SAMPLE_RATE * 2;
    *(uint16_t *)(header + 32) = 2;
    *(uint16_t *)(header + 34) = 16;
    memcpy(header + 36, "data", 4);
    *(uint32_t *)(header + 40) = (uint32_t)pcm_bytes;
}

/* JSON 字符串转义（UTF-8 原样透传），返回写入长度。 */
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

/* multipart 流式上传 WAV → /v1/voice/asr，返回识别文本。 */
static esp_err_t chat_asr(const char *url, const char *bearer, const char *device_id,
                          const int16_t *pcm, size_t pcm_bytes,
                          char *recognized, size_t recognized_cap)
{
    static const char part1[] =
        "--" CHAT_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    static const char part2[] = "\r\n--" CHAT_BOUNDARY "--\r\n";
    uint8_t wav_header[44];
    chat_build_wav_header(wav_header, pcm_bytes);

    char response[2048];
    chat_http_response_t resp = { .data = response, .capacity = sizeof(response) };
    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CHAT_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = chat_http_event,
        .user_data = &resp,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=" CHAT_BOUNDARY);
    esp_http_client_set_header(client, "Authorization", bearer);
    esp_http_client_set_header(client, "X-Device-Id", device_id);

    size_t total = sizeof(part1) - 1 + sizeof(wav_header) + pcm_bytes + sizeof(part2) - 1;
    esp_err_t err = esp_http_client_open(client, (int)total);
    if (err == ESP_OK) {
        if (esp_http_client_write(client, part1, sizeof(part1) - 1) < 0 ||
            esp_http_client_write(client, (const char *)wav_header, sizeof(wav_header)) < 0 ||
            esp_http_client_write(client, (const char *)pcm, (int)pcm_bytes) < 0 ||
            esp_http_client_write(client, part2, sizeof(part2) - 1) < 0) {
            err = ESP_FAIL;
        } else {
            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            char drain[128];
            while (esp_http_client_read(client, drain, sizeof(drain)) > 0) {}
            if (status < 200 || status >= 300 || resp.overflow) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(resp.data);
    const char *text = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "text")) : NULL;
    if (!text) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    strlcpy(recognized, text, recognized_cap);
    cJSON_Delete(root);
    return ESP_OK;
}

/* JSON 提交对话历史 → /v1/voice/chat，返回 AI 回复。 */
static esp_err_t chat_ask(const char *url, const char *bearer, const char *device_id,
                          const chat_message_t *messages, size_t count,
                          char *reply, size_t reply_cap)
{
    char body[4096];
    size_t pos = 0;
    pos += (size_t)snprintf(body + pos, sizeof(body) - pos, "{\"messages\":[");
    for (size_t i = 0; i < count; i++) {
        int written = snprintf(body + pos, sizeof(body) - pos, "%s{\"role\":\"%s\",\"content\":\"",
                               i ? "," : "", messages[i].role);
        if (written < 0 || (size_t)written >= sizeof(body) - pos) return ESP_ERR_INVALID_SIZE;
        pos += (size_t)written;
        pos += chat_json_escape(body + pos, sizeof(body) - pos, messages[i].content);
        int tail = snprintf(body + pos, sizeof(body) - pos, "\"}");
        if (tail < 0 || (size_t)tail >= sizeof(body) - pos) return ESP_ERR_INVALID_SIZE;
        pos += (size_t)tail;
    }
    int last = snprintf(body + pos, sizeof(body) - pos, "]}");
    if (last < 0 || (size_t)last >= sizeof(body) - pos) return ESP_ERR_INVALID_SIZE;
    pos += (size_t)last;

    char response[2048];
    chat_http_response_t resp = { .data = response, .capacity = sizeof(response) };
    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CHAT_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = chat_http_event,
        .user_data = &resp,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", bearer);
    esp_http_client_set_header(client, "X-Device-Id", device_id);
    esp_http_client_set_post_field(client, body, (int)pos);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300 || resp.overflow) return ESP_ERR_INVALID_RESPONSE;

    cJSON *root = cJSON_Parse(resp.data);
    const char *text = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "reply")) : NULL;
    if (!text) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    strlcpy(reply, text, reply_cap);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 请求 /v1/voice/tts，把响应里的 base64 MP3 就地解码到 out，返回 MP3 长度。 */
static esp_err_t chat_tts(const char *url, const char *bearer, const char *device_id,
                          const char *text, uint8_t *out, size_t out_cap, size_t *mp3_len)
{
    char body[CHAT_AI_TEXT_MAX + 64];
    int head = snprintf(body, sizeof(body), "{\"text\":\"");
    if (head < 0 || (size_t)head >= sizeof(body)) return ESP_ERR_INVALID_SIZE;
    size_t pos = (size_t)head;
    pos += chat_json_escape(body + pos, sizeof(body) - pos, text);
    int tail = snprintf(body + pos, sizeof(body) - pos, "\"}");
    if (tail < 0 || (size_t)tail >= sizeof(body) - pos) return ESP_ERR_INVALID_SIZE;
    pos += (size_t)tail;

    chat_http_response_t resp = { .data = (char *)out, .capacity = out_cap };
    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CHAT_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = chat_http_event,
        .user_data = &resp,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", bearer);
    esp_http_client_set_header(client, "X-Device-Id", device_id);
    esp_http_client_set_post_field(client, body, (int)pos);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300 || resp.overflow || resp.length == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 手工提取 "audio":"data:audio/mpeg;base64,XXXX"（避免 cJSON 复制大字符串）。 */
    static const char marker[] = "\"audio\":\"data:audio/mpeg;base64,";
    char *b64 = strstr(resp.data, marker);
    if (!b64) return ESP_ERR_INVALID_RESPONSE;
    b64 += sizeof(marker) - 1;
    char *end = strchr(b64, '"');
    if (!end) return ESP_ERR_INVALID_RESPONSE;
    size_t decoded = 0;
    int rc = mbedtls_base64_decode(out, out_cap, &decoded,
                                   (const unsigned char *)b64, (size_t)(end - b64));
    if (rc != 0 || decoded == 0) return ESP_ERR_INVALID_RESPONSE;
    *mp3_len = decoded;
    return ESP_OK;
}

/* helix 软解 MP3 → PCM，边解边写入 ES8311。 */
static esp_err_t chat_play_mp3(const uint8_t *mp3, size_t len)
{
    if (!s_mp3) {
        s_mp3 = MP3InitDecoder();
        if (!s_mp3) return ESP_ERR_NO_MEM;
    }
    bsp_audio_set_volume(app_settings_get_volume_percent());
    static int16_t pcm[1152 * 2];
    unsigned char *cursor = (unsigned char *)mp3;
    int bytes_left = (int)len;
    bool format_set = false;
    while (bytes_left > 0) {
        int offset = MP3FindSyncWord(cursor, bytes_left);
        if (offset < 0) break;
        cursor += offset;
        bytes_left -= offset;
        unsigned char *in = cursor;
        int in_left = bytes_left;
        int err = MP3Decode(s_mp3, &in, &in_left, pcm, 0);
        int consumed = bytes_left - in_left;
        cursor += consumed;
        bytes_left = in_left;
        if (err == ERR_MP3_NONE) {
            MP3FrameInfo info;
            MP3GetLastFrameInfo(s_mp3, &info);
            if (!format_set) {
                uint8_t channels = info.nChans > 1 ? 2 : 1;
                if (bsp_audio_set_format((uint32_t)info.samprate, 16, channels) != ESP_OK) {
                    return ESP_FAIL;
                }
                format_set = true;
            }
            size_t pcm_bytes = (size_t)info.outputSamps * (info.nChans > 1 ? 2u : 1u) *
                               sizeof(int16_t);
            if (bsp_audio_write(pcm, pcm_bytes) != ESP_OK) break;
        } else if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            break;
        } else if (consumed == 0 && bytes_left > 0) {
            cursor++;
            bytes_left--;
        }
    }
    return ESP_OK;
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

static void chat_task(void *arg)
{
    (void)arg;
    char url[160];
    char bearer[KIRO_PASSPORT_CREDENTIAL_MAX + 8];
    char recognized[CHAT_AI_TEXT_MAX];
    char reply[CHAT_AI_TEXT_MAX];

    for (;;) {
        chat_state_t st = s_state;
        if (st == CHAT_IDLE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (st == CHAT_RECORDING) {
            chat_set_status("聆听中… (再按 OK 停止)");
            if (bsp_audio_set_format(CHAT_SAMPLE_RATE, 16, 1) != ESP_OK) {
                chat_reset_to_idle("音频初始化失败");
                continue;
            }
            bsp_audio_set_volume(app_settings_get_volume_percent());
            s_record_len = 0;
            while (s_record_len < s_record_cap) {
                if (s_stop_record) break;
                size_t want = 512 * sizeof(int16_t);
                if (s_record_len + want > s_record_cap) want = s_record_cap - s_record_len;
                if (bsp_audio_read((uint8_t *)s_record_buf + s_record_len, want) != ESP_OK) break;
                s_record_len += want;
            }
            bsp_audio_close();
            if (s_record_len < 128 * sizeof(int16_t)) {
                chat_reset_to_idle("没听清，再试一次");
                continue;
            }
            s_state = CHAT_ASR;
            continue;
        }

        kiro_passport_network_config_t cfg;
        kiro_passport_network_get_config(&cfg);
        if (!cfg.credential[0]) {
            chat_reset_to_idle("未配对：设置 → Relay");
            continue;
        }
        int blen = snprintf(bearer, sizeof(bearer), "Bearer %s", cfg.credential);
        if (blen <= 0 || blen >= (int)sizeof(bearer)) {
            chat_reset_to_idle("凭证异常");
            continue;
        }

        if (st == CHAT_ASR) {
            chat_set_status("识别中…");
            chat_build_url(cfg.relay_url, "/v1/voice/asr", url, sizeof(url));
            if (chat_asr(url, bearer, cfg.device_id, s_record_buf, s_record_len,
                         recognized, sizeof(recognized)) != ESP_OK) {
                chat_reset_to_idle("识别失败");
                continue;
            }
            free(s_record_buf);
            s_record_buf = NULL;
            s_record_cap = 0;
            s_record_len = 0;
            snprintf(s_transcript, sizeof(s_transcript), "你: %s\nAI: …", recognized);
            chat_show_transcript();
            chat_push_history("user", recognized);
            s_state = CHAT_CHATTING;
            continue;
        }

        if (st == CHAT_CHATTING) {
            chat_set_status("AI 思考中…");
            chat_build_url(cfg.relay_url, "/v1/voice/chat", url, sizeof(url));
            if (chat_ask(url, bearer, cfg.device_id, s_history, s_history_count,
                         reply, sizeof(reply)) != ESP_OK) {
                chat_reset_to_idle("AI 调用失败");
                continue;
            }
            chat_push_history("assistant", reply);
            snprintf(s_transcript, sizeof(s_transcript), "你: %s\nAI: %s", recognized, reply);
            chat_show_transcript();
            s_state = CHAT_TTS;
            continue;
        }

        if (st == CHAT_TTS) {
            chat_set_status("合成语音中…");
            if (!s_tts_buf) {
                s_tts_buf = malloc(CHAT_TTS_BUF_SIZE);
                if (!s_tts_buf) {
                    chat_reset_to_idle("内存不足");
                    continue;
                }
            }
            chat_build_url(cfg.relay_url, "/v1/voice/tts", url, sizeof(url));
            if (chat_tts(url, bearer, cfg.device_id, reply, s_tts_buf,
                         CHAT_TTS_BUF_SIZE, &s_mp3_len) != ESP_OK) {
                chat_reset_to_idle("语音合成失败");
                continue;
            }
            s_state = CHAT_PLAYING;
            continue;
        }

        if (st == CHAT_PLAYING) {
            chat_set_status("朗读中… (长按 OK 退出)");
            chat_marquee_start(reply);
            chat_play_mp3(s_tts_buf, s_mp3_len);
            chat_marquee_stop();
            bsp_audio_close();
            chat_reset_to_idle("OK: 录音  ·  长按 OK: 退出");
            continue;
        }
    }
}

void demo_chat_enter(void)
{
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
    if (!cfg.credential[0]) {
        lv_label_set_text(s_status, "未配对：设置 → Relay 先配对");
    } else if (wifi_manager_get_state() != WIFI_MANAGER_CONNECTED) {
        lv_label_set_text(s_status, "等待 Wi-Fi 连接…");
    } else {
        lv_label_set_text(s_status, "按 OK 开始录音");
    }

    s_transcript[0] = '\0';
    s_state = CHAT_IDLE;
    s_stop_record = false;
    if (!s_task) {
        if (xTaskCreate(chat_task, "demo_chat", 6144, NULL, 4, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "任务创建失败");
        }
    }
    lv_screen_load(s_scr);
}

void demo_chat_exit(void)
{
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
    if (s_record_buf) {
        free(s_record_buf);
        s_record_buf = NULL;
        s_record_cap = 0;
        s_record_len = 0;
    }
    if (s_tts_buf) {
        free(s_tts_buf);
        s_tts_buf = NULL;
        s_mp3_len = 0;
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
        if (!s_record_buf) {
            s_record_buf = malloc(CHAT_RECORD_BYTES);
            s_record_cap = s_record_buf ? CHAT_RECORD_BYTES : 0;
            if (!s_record_buf) {
                s_record_buf = malloc(CHAT_RECORD_FALLBACK_BYTES);
                s_record_cap = s_record_buf ? CHAT_RECORD_FALLBACK_BYTES : 0;
            }
            if (!s_record_buf) {
                chat_set_status("内存不足");
                return;
            }
            ESP_LOGI(TAG, "录音缓冲 %u 字节", (unsigned)s_record_cap);
        }
        s_stop_record = false;
        s_state = CHAT_RECORDING;
    } else if (s_state == CHAT_RECORDING && btn == BSP_BTN_OK) {
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
