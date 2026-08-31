// main/demo_chat.c —— Chat 语音助手：录音 → ASR → AI 对话 → TTS → 播放 全流程。
// 云端复用 relay 的 /v1/voice/asr|chat|tts（ws.yanyun.asia），鉴权用设备凭证
// （Authorization: Bearer 设备凭证 + X-Device-Id），与 Kiro WebSocket 同一套凭证。
//
// 内存设计（ESP32-C3 无 PSRAM，静态 DRAM 已占 ~77%，空闲堆仅数十 KB）：
// 录音 PCM 与 TTS MP3 全部流式读写到专用 chatrec flash 分区，RAM 只保留
// 2KB~8KB 的搬运缓冲，避免大块 malloc；MP3 解码器（~25-30KB）仅在播放阶段持有。
#include "demo.h"
#include "app_settings.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "kiro_passport_network.h"
#include "ui_font_noto_sc_14.h"
#include "ui_font_noto_sc_20.h"
#include "ui_system.h"
#include "wifi_manager.h"
#include "esp_http_client.h"
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
/* 录音上限 4 秒；flash 暂存区 0..CHAT_REC_MAX_BYTES。
 * 注意：esp_partition_erase_range 要求 size 必须是 SPI_FLASH_SEC_SIZE(4096) 的整数倍，
 * 否则直接返回 ESP_ERR_INVALID_SIZE。按 16k*2B/sample 算 4s=128000B，并非 4096 倍数，
 * 会把整段 flash 擦除（录音/MP3 区）map回 ESP_ERR_INVALID_SIZE 而报"擦除失败"。
 * 因此这里把字节数向上取整对齐到 4096（131072B ≈ 4.096s），录音循环仍用实际采样字节。 */
#define CHAT_REC_MAX_SEC 4
#define CHAT_REC_MAX_BYTES 131072  /* 16kHz*16bit*4s≈128000B，向上对齐到 4096 倍数 131072 */
/* TTS MP3 上限 160KB（≈1 分钟语音）；flash 暂存区从 CHAT_MP3_OFFSET 起。 */
#define CHAT_MP3_MAX_BYTES (160 * 1024)
#define CHAT_MP3_OFFSET CHAT_REC_MAX_BYTES
#define CHAT_STORE_NAME "chatrec"
#define CHAT_STORE_SUBTYPE 0x40
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

typedef struct {
    size_t cursor;      /* 已写入 flash 的偏移 */
    size_t capacity;    /* 允许写入的最大偏移 */
    bool overflow;
} chat_stream_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_status;
static lv_obj_t *s_log;
static TaskHandle_t s_task;
static volatile chat_state_t s_state = CHAT_IDLE;
static volatile bool s_stop_record;
static const esp_partition_t *s_store;
static size_t s_rec_len;   /* 本轮到 flash 的 PCM 字节数 */
static size_t s_mp3_len;   /* 本轮到 flash 的 MP3 字节数 */
static chat_message_t s_history[CHAT_HISTORY_MAX];
static size_t s_history_count;
static char s_transcript[CHAT_AI_TEXT_MAX * 2 + 32];
static HMP3Decoder s_mp3;
/* 底部滚动字幕条：TTS 播放时滚动 AI 回复。 */
static lv_obj_t *s_marquee_bar;
static lv_obj_t *s_marquee_label;

/* --- 内存优化（ESP32-C3 无 PSRAM）----------
 * chat_asr/chat_ask/chat_play_mp3_stream 内的大 HTTP 请求/响应/MP3 缓冲原先声明在
 * 各自函数栈上，导致 chat_task 峰值栈高达约 8KB，而进入 Chat 段时系统堆被 WiFi/TLS/
 * LVGL 占用殆尽，8KB 的连续任务栈分配失败（日志"任务创建失败"）。
 *
 * 这些大缓冲既不能放任务栈（栈要小），也不能永久放静态区（.bss 会在启动时就永久
 * 占据内存、进一步榨干本就紧张的系统堆）。因此改为在任务运行期间 transiently 用
 * malloc 分配、用毕即 free：缓冲仅在整个 HTTP/播放处理期间短暂存在，不常驻堆，
 * 也不会撑爆任务栈。这样 chat_task 只用 ~3KB 栈，创建任务成功率大幅提高。 */
#define CHAT_HTTP_BODY_MAX 4096
#define CHAT_HTTP_RESPONSE_MAX 2048
#define CHAT_ASR_FILEBUF_MAX 2048
#define CHAT_MP3_READBUF_MAX 8192

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

/* 查找 chatrec 暂存分区；旧分区表上没有时给出明确错误。 */
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

/* 顺序擦除一段 flash（offset/size 必须 4KB 对齐）。 */
static esp_err_t chat_store_erase(size_t offset, size_t size)
{
    return esp_partition_erase_range(s_store, offset, size);
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

/* TTS 响应事件：直接把 MP3 字节流写入 flash 暂存区（无大 RAM 缓冲）。 */
static esp_err_t chat_stream_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data || event->data_len <= 0) {
        return ESP_OK;
    }
    chat_stream_t *stream = event->user_data;
    if (stream->cursor + (size_t)event->data_len > stream->capacity) {
        stream->overflow = true;
        return ESP_FAIL;
    }
    esp_err_t err = esp_partition_write(s_store, stream->cursor, event->data, event->data_len);
    if (err == ESP_OK) stream->cursor += (size_t)event->data_len;
    return err;
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

/* multipart 流式上传 flash 里的 WAV → /v1/voice/asr，返回识别文本。 */
static esp_err_t chat_asr(const char *url, const char *bearer, const char *device_id,
                          size_t pcm_bytes, char *recognized, size_t recognized_cap)
{
    static const char part1[] =
        "--" CHAT_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    static const char part2[] = "\r\n--" CHAT_BOUNDARY "--\r\n";
    uint8_t wav_header[44];
    chat_build_wav_header(wav_header, pcm_bytes);

    /* transient heap：不撑任务栈，也不永久占堆（见文件顶部内存优化说明） */
    char *response = malloc(CHAT_HTTP_RESPONSE_MAX);
    uint8_t *filebuf = malloc(CHAT_ASR_FILEBUF_MAX);
    if (!response || !filebuf) {
        free(response);
        free(filebuf);
        return ESP_ERR_NO_MEM;
    }
    chat_http_response_t resp = { .data = response, .capacity = CHAT_HTTP_RESPONSE_MAX };
    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CHAT_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = chat_http_event,
        .user_data = &resp,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(response);
        free(filebuf);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=" CHAT_BOUNDARY);
    esp_http_client_set_header(client, "Authorization", bearer);
    esp_http_client_set_header(client, "X-Device-Id", device_id);

    size_t total = sizeof(part1) - 1 + sizeof(wav_header) + pcm_bytes + sizeof(part2) - 1;
    esp_err_t err = esp_http_client_open(client, (int)total);
    if (err == ESP_OK) {
        bool ok = esp_http_client_write(client, part1, sizeof(part1) - 1) >= 0 &&
                  esp_http_client_write(client, (const char *)wav_header, sizeof(wav_header)) >= 0;
        size_t off = 0;
        while (ok && off < pcm_bytes) {
            size_t n = pcm_bytes - off;
            if (n > CHAT_ASR_FILEBUF_MAX) n = CHAT_ASR_FILEBUF_MAX;
            if (esp_partition_read(s_store, off, filebuf, n) != ESP_OK) {
                ok = false;
                break;
            }
            if (esp_http_client_write(client, (const char *)filebuf, (int)n) < 0) {
                ok = false;
                break;
            }
            off += n;
        }
        if (ok) ok = esp_http_client_write(client, part2, sizeof(part2) - 1) >= 0;
        if (!ok) {
            err = ESP_FAIL;
        } else {
            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            resp.length = 0;
            int rlen = 0;
            while (resp.length + 1 < resp.capacity &&
                   (rlen = esp_http_client_read(client, resp.data + resp.length,
                                               (int)(resp.capacity - 1 - resp.length))) > 0) {
                resp.length += (size_t)rlen;
            }
            if (resp.length < resp.capacity) {
                resp.data[resp.length] = '\0';
            } else {
                resp.data[resp.capacity - 1] = '\0';
                resp.overflow = true;
            }
            ESP_LOGI(TAG, "ASR http status=%d err=%d resp_len=%u ovf=%d resp=%.160s",
                     status, (int)err, (unsigned)resp.length, (int)resp.overflow, resp.data);
            if (status < 200 || status >= 300 || resp.overflow) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    ESP_LOGI(TAG, "ASR open err=%d (errno) rec_len=%u free=%lu min=%lu",
             (int)err, (unsigned)pcm_bytes,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ASR 失败 err=%d", (int)err);
        free(response);
        free(filebuf);
        return err;
    }

    cJSON *root = cJSON_Parse(resp.data);
    const char *text = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "text")) : NULL;
    ESP_LOGI(TAG, "ASR resp=%.160s text=%s", resp.data, text ? text : "(null)");
    if (!text || text[0] == '\0') {
        cJSON_Delete(root);
        free(response);
        free(filebuf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    strlcpy(recognized, text, recognized_cap);
    cJSON_Delete(root);
    free(response);
    free(filebuf);
    return ESP_OK;
}

/* JSON 提交对话历史 → /v1/voice/chat，返回 AI 回复。 */
static esp_err_t chat_ask(const char *url, const char *bearer, const char *device_id,
                          const chat_message_t *messages, size_t count,
                          char *reply, size_t reply_cap)
{
    /* transient heap：不撑任务栈，也不永久占堆（见文件顶部内存优化说明） */
    char *body = malloc(CHAT_HTTP_BODY_MAX);
    char *response = malloc(CHAT_HTTP_RESPONSE_MAX);
    if (!body || !response) {
        free(body);
        free(response);
        return ESP_ERR_NO_MEM;
    }
    const size_t body_cap = CHAT_HTTP_BODY_MAX;
    size_t pos = 0;
    pos += (size_t)snprintf(body + pos, body_cap - pos, "{\"messages\":[");
    for (size_t i = 0; i < count; i++) {
        int written = snprintf(body + pos, body_cap - pos, "%s{\"role\":\"%s\",\"content\":\"",
                               i ? "," : "", messages[i].role);
        if (written < 0 || (size_t)written >= body_cap - pos) { free(body); free(response); return ESP_ERR_INVALID_SIZE; }
        pos += (size_t)written;
        pos += chat_json_escape(body + pos, body_cap - pos, messages[i].content);
        int tail = snprintf(body + pos, body_cap - pos, "\"}");
        if (tail < 0 || (size_t)tail >= body_cap - pos) { free(body); free(response); return ESP_ERR_INVALID_SIZE; }
        pos += (size_t)tail;
    }
    int last = snprintf(body + pos, body_cap - pos, "]}");
    if (last < 0 || (size_t)last >= body_cap - pos) { free(body); free(response); return ESP_ERR_INVALID_SIZE; }
    pos += (size_t)last;

    chat_http_response_t resp = { .data = response, .capacity = CHAT_HTTP_RESPONSE_MAX };
    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CHAT_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = chat_http_event,
        .user_data = &resp,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(body); free(response); return ESP_ERR_NO_MEM; }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", bearer);
    esp_http_client_set_header(client, "X-Device-Id", device_id);
    esp_http_client_set_post_field(client, body, (int)pos);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300 || resp.overflow) {
        free(body);
        free(response);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *root = cJSON_Parse(resp.data);
    const char *text = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "reply")) : NULL;
    if (!text) {
        cJSON_Delete(root);
        free(body);
        free(response);
        return ESP_ERR_INVALID_RESPONSE;
    }
    strlcpy(reply, text, reply_cap);
    cJSON_Delete(root);
    free(body);
    free(response);
    return ESP_OK;
}

/* 请求 /v1/voice/tts（Accept: audio/mpeg），把 MP3 字节流直接写入 flash 暂存区。 */
static esp_err_t chat_tts_stream(const char *url, const char *bearer, const char *device_id,
                                 const char *text)
{
    char body[CHAT_AI_TEXT_MAX + 64];
    int head = snprintf(body, sizeof(body), "{\"text\":\"");
    if (head < 0 || (size_t)head >= sizeof(body)) return ESP_ERR_INVALID_SIZE;
    size_t pos = (size_t)head;
    pos += chat_json_escape(body + pos, sizeof(body) - pos, text);
    int tail = snprintf(body + pos, sizeof(body) - pos, "\"}");
    if (tail < 0 || (size_t)tail >= sizeof(body) - pos) return ESP_ERR_INVALID_SIZE;
    pos += (size_t)tail;

    chat_stream_t stream = {
        .cursor = CHAT_MP3_OFFSET,
        .capacity = CHAT_MP3_OFFSET + CHAT_MP3_MAX_BYTES,
        .overflow = false,
    };
    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CHAT_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = chat_stream_event,
        .user_data = &stream,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "audio/mpeg");
    esp_http_client_set_header(client, "Authorization", bearer);
    esp_http_client_set_header(client, "X-Device-Id", device_id);
    esp_http_client_set_post_field(client, body, (int)pos);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || stream.overflow) return ESP_ERR_INVALID_RESPONSE;
    s_mp3_len = stream.cursor - CHAT_MP3_OFFSET;
    return s_mp3_len > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/* helix 软解 flash 里的 MP3（分块读入）→ PCM → ES8311。 */
static esp_err_t chat_play_mp3_stream(void)
{
    if (!s_mp3) {
        s_mp3 = MP3InitDecoder();
        if (!s_mp3) return ESP_ERR_NO_MEM;
    }
    bsp_audio_set_volume(app_settings_get_volume_percent());
    static int16_t pcm[1152 * 2];
    uint8_t *buf = malloc(CHAT_MP3_READBUF_MAX);  /* transient heap：不撑任务栈也不常驻堆 */
    if (!buf) return ESP_ERR_NO_MEM;
    esp_err_t result = ESP_OK;
    const size_t buf_cap = CHAT_MP3_READBUF_MAX;
    size_t buf_len = 0;
    size_t flash_off = 0;
    bool format_set = false;

    while (true) {
        if (buf_len < buf_cap && flash_off < s_mp3_len) {
            size_t want = buf_cap - buf_len;
            if (want > s_mp3_len - flash_off) want = s_mp3_len - flash_off;
            if (esp_partition_read(s_store, CHAT_MP3_OFFSET + flash_off, buf + buf_len, want) != ESP_OK) {
                break;
            }
            flash_off += want;
            buf_len += want;
        }
        if (buf_len == 0) break;

        int offset = MP3FindSyncWord(buf, (int)buf_len);
        if (offset < 0) break;
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
            }
            size_t pcm_bytes = (size_t)info.outputSamps * (info.nChans > 1 ? 2u : 1u) *
                               sizeof(int16_t);
            if (bsp_audio_write(pcm, pcm_bytes) != ESP_OK) break;
        } else if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            /* 数据不足：flash 还有就继续补充，否则视为播放完毕。 */
            if (flash_off >= s_mp3_len) break;
        } else if (in_left == before) {
            /* 帧头损坏且无进展：跳 1 字节。 */
            if (buf_len > 0) {
                memmove(buf, buf + 1, buf_len - 1);
                buf_len--;
            } else {
                break;
            }
        }
    }
    free(buf);
    return result;
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
            /* 每 ~2s 打印一次任务栈高水位，便于验证栈大小是否足够。 */
            static uint32_t idle_counter;
            if ((++idle_counter % 40) == 0) {
                ESP_LOGI(TAG, "stack high-water=%lu bytes (free heap=%lu)",
                         (unsigned long)uxTaskGetStackHighWaterMark(NULL),
                         (unsigned long)esp_get_free_heap_size());
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (st == CHAT_RECORDING) {
            if (chat_store_init() != ESP_OK) {
                chat_reset_to_idle("存储分区缺失");
                continue;
            }
            /* 预擦除录音区（~2s），录音期间不再写/擦 flash，保证采样不丢。 */
            chat_set_status("准备录音…");
            esp_err_t erase_err = chat_store_erase(0, CHAT_REC_MAX_BYTES);
            ESP_LOGI(TAG, "erase(off=%u,sz=%u) -> err=%d free=%lu min=%lu",
                     (unsigned)0, (unsigned)(CHAT_REC_MAX_BYTES), (int)erase_err,
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size());
            if (erase_err != ESP_OK) {
                chat_reset_to_idle("存储擦除失败");
                continue;
            }
            chat_set_status("聆听中… (再按 OK 停止)");
            if (bsp_audio_set_format(CHAT_SAMPLE_RATE, 16, 1) != ESP_OK) {
                chat_reset_to_idle("音频初始化失败");
                continue;
            }
            bsp_audio_set_volume(app_settings_get_volume_percent());
            size_t cursor = 0;
            s_rec_len = 0;
            int16_t chunk[1024];
            while (s_rec_len < CHAT_REC_MAX_BYTES) {
                if (s_stop_record) break;
                if (bsp_audio_read(chunk, sizeof(chunk)) != ESP_OK) break;
                if (esp_partition_write(s_store, cursor, chunk, sizeof(chunk)) != ESP_OK) break;
                cursor += sizeof(chunk);
                s_rec_len += sizeof(chunk);
            }
            bsp_audio_close();
            if (s_rec_len < 128 * sizeof(int16_t)) {
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
            chat_log_heap("asr_start");
            chat_build_url(cfg.relay_url, "/v1/voice/asr", url, sizeof(url));
            esp_err_t asr_err = chat_asr(url, bearer, cfg.device_id, s_rec_len,
                                         recognized, sizeof(recognized));
            if (asr_err != ESP_OK) {
                chat_reset_to_idle("识别失败");
                continue;
            }
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
            chat_log_heap("tts_start");
            /* 预擦除 MP3 区（~2.5s），下载时直接顺序写 flash。 */
            if (chat_store_erase(CHAT_MP3_OFFSET, CHAT_MP3_MAX_BYTES) != ESP_OK) {
                chat_reset_to_idle("存储擦除失败");
                continue;
            }
            chat_build_url(cfg.relay_url, "/v1/voice/tts", url, sizeof(url));
            if (chat_tts_stream(url, bearer, cfg.device_id, reply) != ESP_OK) {
                chat_reset_to_idle("语音合成失败");
                continue;
            }
            s_state = CHAT_PLAYING;
            continue;
        }

        if (st == CHAT_PLAYING) {
            chat_set_status("朗读中… (长按 OK 退出)");
            chat_log_heap("playing_start");
            chat_marquee_start(reply);
            chat_play_mp3_stream();
            chat_marquee_stop();
            bsp_audio_close();
            /* 播放结束立即释放解码器：录音阶段不常驻。 */
            if (s_mp3) {
                MP3FreeDecoder(s_mp3);
                s_mp3 = NULL;
            }
            chat_log_heap("playing_end");
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
    } else if (chat_store_init() != ESP_OK) {
        lv_label_set_text(s_status, "存储分区缺失，请更新固件");
    } else {
        lv_label_set_text(s_status, "按 OK 开始录音");
    }

    s_transcript[0] = '\0';
    s_state = CHAT_IDLE;
    s_stop_record = false;
    if (!s_task) {
        /* 大缓冲已移出任务栈（transient heap），但 HTTPS ASR/TLS 链路深，实测 ASR 阶段
         * 峰值栈 ~6-7KB，故给 8192 字节。创建只分配一个空栈体（不再是 8KB 里塞 14KB
         * 缓冲），在 ~7KB 空闲堆下也能稳定创建其栈。 */
        if (xTaskCreate(chat_task, "demo_chat", 8192, NULL, 4, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "任务创建失败");
        }
    }
    chat_log_heap("chat_enter");
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
