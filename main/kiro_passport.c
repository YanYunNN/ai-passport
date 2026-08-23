#include "kiro_passport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define KIRO_REQUEST_EXPIRY_MAX_SECONDS 300
#define KIRO_REPLAY_CACHE_SIZE 8

typedef struct {
    char type[12];
    char device_id[KIRO_PASSPORT_DEVICE_ID_MAX];
    char session_id[KIRO_PASSPORT_SESSION_ID_MAX];
    char request_id[KIRO_PASSPORT_REQUEST_ID_MAX];
    char tool[KIRO_PASSPORT_TOOL_MAX];
    char summary[KIRO_PASSPORT_SUMMARY_MAX];
    time_t expires_at;
} request_message_t;

typedef struct {
    char request_id[KIRO_PASSPORT_REQUEST_ID_MAX];
    time_t expires_at;
} replay_entry_t;

static SemaphoreHandle_t s_mutex;
static kiro_passport_snapshot_t s_snapshot;
static char s_device_id[KIRO_PASSPORT_DEVICE_ID_MAX];
static char s_session_id[KIRO_PASSPORT_SESSION_ID_MAX];
static char s_request_session_id[KIRO_PASSPORT_SESSION_ID_MAX];
static time_t s_expires_at;
static kiro_passport_decision_t s_decision;
static bool s_decision_pending;
static replay_entry_t s_replay_cache[KIRO_REPLAY_CACHE_SIZE];

static void lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static bool copy_safe(char *out, size_t out_size, const char *value)
{
    if (!out || !value || !out_size) return false;
    size_t length = strlen(value);
    if (!length || length >= out_size) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char character = (unsigned char)value[i];
        if (character < 0x20 || character > 0x7e || character == '"' ||
            character == '\\') return false;
    }
    memcpy(out, value, length + 1);
    return true;
}

static void skip_space(const char **cursor)
{
    while (isspace((unsigned char)**cursor)) (*cursor)++;
}

static bool read_string(const char **cursor, char *out, size_t out_size)
{
    skip_space(cursor);
    if (*(*cursor)++ != '"') return false;

    size_t length = 0;
    while (**cursor && **cursor != '"') {
        unsigned char character = (unsigned char)**cursor;
        if (character < 0x20 || character > 0x7e || character == '\\' ||
            length + 1 >= out_size) return false;
        out[length++] = *(*cursor)++;
    }
    if (**cursor != '"') return false;
    (*cursor)++;
    out[length] = '\0';
    return length != 0;
}

static bool read_uint(const char **cursor, time_t *out)
{
    skip_space(cursor);
    if (!isdigit((unsigned char)**cursor)) return false;

    uint64_t value = 0;
    while (isdigit((unsigned char)**cursor)) {
        uint8_t digit = (uint8_t)(**cursor - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
        (*cursor)++;
    }
    if (value > INT64_MAX) return false;
    *out = (time_t)value;
    return true;
}

static bool parse_request(const char *message, request_message_t *request)
{
    if (!message || !request || strlen(message) > 511) return false;
    memset(request, 0, sizeof(*request));

    enum { FIELD_VERSION = 1, FIELD_TYPE = 2, FIELD_DEVICE = 4, FIELD_SESSION = 8,
           FIELD_REQUEST = 16, FIELD_TOOL = 32, FIELD_SUMMARY = 64, FIELD_EXPIRY = 128 };
    unsigned fields = 0;
    const char *cursor = message;
    skip_space(&cursor);
    if (*cursor++ != '{') return false;

    for (;;) {
        char key[16];
        skip_space(&cursor);
        if (*cursor == '}') {
            cursor++;
            break;
        }
        if (!read_string(&cursor, key, sizeof(key))) return false;
        skip_space(&cursor);
        if (*cursor++ != ':') return false;

        unsigned field = 0;
        bool parsed = false;
        if (strcmp(key, "v") == 0) {
            time_t version;
            field = FIELD_VERSION;
            parsed = read_uint(&cursor, &version) && version == 1;
        } else if (strcmp(key, "type") == 0) {
            field = FIELD_TYPE;
            parsed = read_string(&cursor, request->type, sizeof(request->type));
        } else if (strcmp(key, "device_id") == 0) {
            field = FIELD_DEVICE;
            parsed = read_string(&cursor, request->device_id, sizeof(request->device_id));
        } else if (strcmp(key, "session_id") == 0) {
            field = FIELD_SESSION;
            parsed = read_string(&cursor, request->session_id, sizeof(request->session_id));
        } else if (strcmp(key, "request_id") == 0) {
            field = FIELD_REQUEST;
            parsed = read_string(&cursor, request->request_id, sizeof(request->request_id));
        } else if (strcmp(key, "tool") == 0) {
            field = FIELD_TOOL;
            parsed = read_string(&cursor, request->tool, sizeof(request->tool));
        } else if (strcmp(key, "summary") == 0) {
            field = FIELD_SUMMARY;
            parsed = read_string(&cursor, request->summary, sizeof(request->summary));
        } else if (strcmp(key, "expires_at") == 0) {
            field = FIELD_EXPIRY;
            parsed = read_uint(&cursor, &request->expires_at);
        } else {
            return false;
        }
        if (!parsed || (fields & field)) return false;
        fields |= field;

        skip_space(&cursor);
        if (*cursor == '}') {
            cursor++;
            break;
        }
        if (*cursor++ != ',') return false;
    }
    skip_space(&cursor);
    return !*cursor && fields == (FIELD_VERSION | FIELD_TYPE | FIELD_DEVICE |
        FIELD_SESSION | FIELD_REQUEST | FIELD_TOOL | FIELD_SUMMARY | FIELD_EXPIRY) &&
        strcmp(request->type, "request") == 0;
}

static void queue_decision_locked(bool allow)
{
    s_decision = (kiro_passport_decision_t){ .allow = allow };
    snprintf(s_decision.device_id, sizeof(s_decision.device_id), "%s", s_device_id);
    snprintf(s_decision.session_id, sizeof(s_decision.session_id), "%s", s_request_session_id);
    snprintf(s_decision.request_id, sizeof(s_decision.request_id), "%s", s_snapshot.request_id);
    s_decision_pending = true;
    snprintf(s_snapshot.decision, sizeof(s_snapshot.decision), "%s", allow ? "allow" : "deny");
    s_snapshot.pending = false;
    s_snapshot.tool[0] = '\0';
    s_snapshot.summary[0] = '\0';
    s_expires_at = 0;
    snprintf(s_snapshot.state, sizeof(s_snapshot.state), "%s", "idle");
}

static bool replay_seen_locked(const char *request_id, time_t now)
{
    for (size_t i = 0; i < KIRO_REPLAY_CACHE_SIZE; i++) {
        if (s_replay_cache[i].expires_at <= now) {
            s_replay_cache[i].request_id[0] = '\0';
            s_replay_cache[i].expires_at = 0;
        } else if (strcmp(s_replay_cache[i].request_id, request_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool record_request_locked(const char *request_id, time_t expires_at)
{
    for (size_t i = 0; i < KIRO_REPLAY_CACHE_SIZE; i++) {
        if (!s_replay_cache[i].request_id[0]) {
            snprintf(s_replay_cache[i].request_id, sizeof(s_replay_cache[i].request_id), "%s", request_id);
            s_replay_cache[i].expires_at = expires_at;
            return true;
        }
    }
    return false;
}

esp_err_t kiro_passport_init(const char *device_id)
{
    if (!copy_safe(s_device_id, sizeof(s_device_id), device_id)) return ESP_ERR_INVALID_ARG;
    if (s_mutex) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    lock();
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.initialized = true;
    snprintf(s_snapshot.state, sizeof(s_snapshot.state), "%s", "sleep");
    unlock();
    return ESP_OK;
}

void kiro_passport_get_snapshot(kiro_passport_snapshot_t *snapshot)
{
    if (!snapshot) return;
    if (!s_mutex) {
        memset(snapshot, 0, sizeof(*snapshot));
        return;
    }
    lock();
    *snapshot = s_snapshot;
    unlock();
}

void kiro_passport_set_connection(bool connected, const char *session_id)
{
    if (!s_mutex) return;
    lock();
    s_snapshot.connected = connected;
    if (connected && copy_safe(s_session_id, sizeof(s_session_id), session_id)) {
        if (!s_snapshot.pending && !s_decision_pending) {
            snprintf(s_snapshot.state, sizeof(s_snapshot.state), "%s", "idle");
        }
    } else if (!connected) {
        /* A physical approval must never survive the authenticated session that created it. */
        if (s_snapshot.pending && !s_decision_pending) queue_decision_locked(false);
        s_session_id[0] = '\0';
        if (!s_snapshot.pending && !s_decision_pending) {
            snprintf(s_snapshot.state, sizeof(s_snapshot.state), "%s", "sleep");
        }
    }
    unlock();
}

kiro_passport_request_result_t kiro_passport_submit_request(
    const char *message, time_t now, kiro_passport_decision_t *rejection)
{
    request_message_t request;
    if (rejection) memset(rejection, 0, sizeof(*rejection));
    if (!parse_request(message, &request) || !s_mutex) return KIRO_PASSPORT_REQUEST_DENIED_INVALID;

    lock();
    kiro_passport_request_result_t result = KIRO_PASSPORT_REQUEST_DENIED_INVALID;
    bool identity_matches = strcmp(request.device_id, s_device_id) == 0 &&
                            s_session_id[0] && strcmp(request.session_id, s_session_id) == 0;
    if (!identity_matches) {
        result = KIRO_PASSPORT_REQUEST_DENIED_INVALID;
    } else if (request.expires_at <= now || request.expires_at > now + KIRO_REQUEST_EXPIRY_MAX_SECONDS) {
        result = KIRO_PASSPORT_REQUEST_DENIED_EXPIRED;
    } else if (replay_seen_locked(request.request_id, now)) {
        result = KIRO_PASSPORT_REQUEST_DENIED_REPLAY;
    } else if (s_snapshot.pending || s_decision_pending ||
               !record_request_locked(request.request_id, request.expires_at)) {
        result = KIRO_PASSPORT_REQUEST_DENIED_BUSY;
    } else {
        s_snapshot.pending = true;
        s_snapshot.decision[0] = '\0';
        snprintf(s_snapshot.state, sizeof(s_snapshot.state), "%s", "attention");
        snprintf(s_snapshot.request_id, sizeof(s_snapshot.request_id), "%s", request.request_id);
        snprintf(s_snapshot.tool, sizeof(s_snapshot.tool), "%s", request.tool);
        snprintf(s_snapshot.summary, sizeof(s_snapshot.summary), "%s", request.summary);
        snprintf(s_request_session_id, sizeof(s_request_session_id), "%s", request.session_id);
        s_expires_at = request.expires_at;
        result = KIRO_PASSPORT_REQUEST_ACCEPTED;
    }
    if (result != KIRO_PASSPORT_REQUEST_ACCEPTED && identity_matches && rejection) {
        *rejection = (kiro_passport_decision_t){ .allow = false };
        snprintf(rejection->device_id, sizeof(rejection->device_id), "%s", request.device_id);
        snprintf(rejection->session_id, sizeof(rejection->session_id), "%s", request.session_id);
        snprintf(rejection->request_id, sizeof(rejection->request_id), "%s", request.request_id);
    }
    unlock();
    return result;
}

void kiro_passport_expire(time_t now)
{
    if (!s_mutex) return;
    lock();
    if (s_snapshot.pending && s_expires_at && now >= s_expires_at) queue_decision_locked(false);
    unlock();
}

esp_err_t kiro_passport_decide(bool allow)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    lock();
    if (!s_snapshot.pending || s_decision_pending) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    queue_decision_locked(allow);
    unlock();
    return ESP_OK;
}

bool kiro_passport_get_decision(kiro_passport_decision_t *decision)
{
    if (!decision || !s_mutex) return false;
    lock();
    bool available = s_decision_pending;
    if (available) *decision = s_decision;
    unlock();
    return available;
}

void kiro_passport_ack_decision(const kiro_passport_decision_t *decision)
{
    if (!decision || !s_mutex) return;
    lock();
    if (s_decision_pending && strcmp(decision->request_id, s_decision.request_id) == 0 &&
        strcmp(decision->session_id, s_decision.session_id) == 0) {
        s_decision_pending = false;
    }
    unlock();
}
