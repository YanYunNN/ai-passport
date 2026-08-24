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

/* Loads the device identity and (only on encrypted NVS builds) enrollment data. */
esp_err_t kiro_passport_network_init(void);
void kiro_passport_network_get_config(kiro_passport_network_config_t *config);

/*
 * Stores an enrollment-issued device credential. Persistence is refused unless
 * NVS encryption is enabled; deployment/API tokens are never accepted here.
 */
esp_err_t kiro_passport_network_configure(const char *relay_url, const char *credential);
esp_err_t kiro_passport_network_clear_configuration(void);

/* Device Code pairing runs asynchronously in the Passport network task. */
bool kiro_passport_network_enrollment_supported(void);
esp_err_t kiro_passport_network_start_enrollment(void);
void kiro_passport_network_cancel_enrollment(void);
void kiro_passport_network_get_enrollment(kiro_passport_enrollment_snapshot_t *snapshot);

kiro_passport_network_state_t kiro_passport_network_get_state(void);
const char *kiro_passport_network_state_name(kiro_passport_network_state_t state);
