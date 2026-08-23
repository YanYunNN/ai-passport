#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define KIRO_PASSPORT_DEVICE_ID_MAX 32
#define KIRO_PASSPORT_SESSION_ID_MAX 48
#define KIRO_PASSPORT_REQUEST_ID_MAX 37
#define KIRO_PASSPORT_TOOL_MAX 32
#define KIRO_PASSPORT_SUMMARY_MAX 72

typedef struct {
    bool initialized;
    bool connected;
    bool pending;
    char state[12];
    char request_id[KIRO_PASSPORT_REQUEST_ID_MAX];
    char tool[KIRO_PASSPORT_TOOL_MAX];
    char summary[KIRO_PASSPORT_SUMMARY_MAX];
    char decision[8];
} kiro_passport_snapshot_t;

typedef struct {
    char device_id[KIRO_PASSPORT_DEVICE_ID_MAX];
    char session_id[KIRO_PASSPORT_SESSION_ID_MAX];
    char request_id[KIRO_PASSPORT_REQUEST_ID_MAX];
    bool allow;
} kiro_passport_decision_t;

typedef enum {
    KIRO_PASSPORT_REQUEST_ACCEPTED,
    KIRO_PASSPORT_REQUEST_DENIED_INVALID,
    KIRO_PASSPORT_REQUEST_DENIED_EXPIRED,
    KIRO_PASSPORT_REQUEST_DENIED_BUSY,
    KIRO_PASSPORT_REQUEST_DENIED_REPLAY,
} kiro_passport_request_result_t;

/* Initializes the transport-independent, thread-safe approval state machine. */
esp_err_t kiro_passport_init(const char *device_id);
void kiro_passport_get_snapshot(kiro_passport_snapshot_t *snapshot);

/* Associates accepted requests with the current authenticated WebSocket session. */
void kiro_passport_set_connection(bool connected, const char *session_id);

/*
 * Accepts only strict v1 request JSON matching this device and active session.
 * The request must carry a unique request_id and an expiry no more than five
 * minutes ahead. Invalid, replayed, expired, and concurrent requests are denied.
 */
kiro_passport_request_result_t kiro_passport_submit_request(
    const char *message, time_t now, kiro_passport_decision_t *rejection);

/* Expires a pending approval and queues a deny decision. */
void kiro_passport_expire(time_t now);

/* Device-side choice from physical buttons. */
esp_err_t kiro_passport_decide(bool allow);

/* A decision remains available until the transport confirms it was sent. */
bool kiro_passport_get_decision(kiro_passport_decision_t *decision);
void kiro_passport_ack_decision(const kiro_passport_decision_t *decision);
