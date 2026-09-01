#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kiro_passport.h"

#define KIRO_PASSPORT_RELAY_URL_MAX 128
#define KIRO_PASSPORT_CREDENTIAL_MAX 192
#define KIRO_PASSPORT_USER_CODE_MAX 7

typedef struct {
    char device_id[KIRO_PASSPORT_DEVICE_ID_MAX];
    char relay_url[KIRO_PASSPORT_RELAY_URL_MAX];
    char credential[KIRO_PASSPORT_CREDENTIAL_MAX];
} kiro_passport_network_config_t;

typedef enum {
    KIRO_PASSPORT_NETWORK_UNCONFIGURED,
    KIRO_PASSPORT_NETWORK_WAITING_WIFI,
    KIRO_PASSPORT_NETWORK_WAITING_CLOCK,
    KIRO_PASSPORT_NETWORK_CONNECTING,
    KIRO_PASSPORT_NETWORK_CONNECTED,
    KIRO_PASSPORT_NETWORK_ERROR,
} kiro_passport_network_state_t;

typedef enum {
    KIRO_PASSPORT_ENROLLMENT_IDLE,
    KIRO_PASSPORT_ENROLLMENT_REQUESTING,
    KIRO_PASSPORT_ENROLLMENT_WAITING_APPROVAL,
    KIRO_PASSPORT_ENROLLMENT_ERROR,
} kiro_passport_enrollment_state_t;

typedef struct {
    kiro_passport_enrollment_state_t state;
    char user_code[KIRO_PASSPORT_USER_CODE_MAX];
    uint32_t expires_in_seconds;
    esp_err_t last_error;
} kiro_passport_enrollment_snapshot_t;

/* Loads the device identity and any persisted enrollment data. */
esp_err_t kiro_passport_network_init(void);
void kiro_passport_network_get_config(kiro_passport_network_config_t *config);

/* Stores an enrollment-issued device credential in NVS; deployment/API tokens are never accepted. */
esp_err_t kiro_passport_network_configure(const char *relay_url, const char *credential);
esp_err_t kiro_passport_network_clear_configuration(void);

/* Device Code pairing runs asynchronously in the Passport network task. */
bool kiro_passport_network_enrollment_supported(void);
esp_err_t kiro_passport_network_start_enrollment(void);
void kiro_passport_network_cancel_enrollment(void);
void kiro_passport_network_get_enrollment(kiro_passport_enrollment_snapshot_t *snapshot);

kiro_passport_network_state_t kiro_passport_network_get_state(void);
const char *kiro_passport_network_state_name(kiro_passport_network_state_t state);

typedef struct {
    char id[64];
    char title[64];
    const uint8_t *data;
    size_t size;
    uint32_t version;
} kiro_passport_image_info_t;

/* 获取当前接收到的图片信息，若无图片返回 false */
bool kiro_passport_network_get_image(kiro_passport_image_info_t *out_info);

/* 内存预算约束：ESP32-C3 无 PSRAM，通知在全局静态区中保存。
 * content 使用 900 字节缓冲：中文按 UTF-8 每字 3 字节，可容纳约 290 字。
 * 由于设备控制缓冲固定为 1024 字节，整体 notify 帧被 worker 严格限制在
 * 1000 字节内，正文实际上限约 850 字节（约 280 汉字）。title 48 字节。 */
typedef struct {
    char id[37];
    char title[48];
    char content[900];
    uint32_t version; /* 每次收到新通知递增 */
    bool present;
} kiro_passport_notify_info_t;

/* 获取最近一次的通知快照，若无已接收通知返回 false */
bool kiro_passport_network_get_notify(kiro_passport_notify_info_t *out_info);

/* 清除当前通知的 present 标志（version 保持不变），用于用户关闭通知 */
void kiro_passport_network_clear_notify(void);

/* 发送自定义文本消息到已连接的 WebSocket (成功返回发送字节数，失败返回 -1) */
int kiro_passport_network_send_text(const char *message);

/* 发送二进制消息到已连接的 WebSocket (成功返回发送字节数，失败返回 -1) */
int kiro_passport_network_send_binary(const void *data, size_t length);

/* 检查 WebSocket 是否处于连接活跃状态 */
bool kiro_passport_network_is_connected(void);

/* 暂停/恢复 WebSocket 连接（用于 Chat 语音助手等高内存场景释放 TLS 堆内存） */
void kiro_passport_network_pause(bool pause);

