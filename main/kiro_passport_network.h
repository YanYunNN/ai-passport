#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kiro_passport.h"

#define KIRO_PASSPORT_RELAY_URL_MAX 128
#define KIRO_PASSPORT_CREDENTIAL_MAX 192

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

/* Loads the device identity and (only on encrypted NVS builds) enrollment data. */
esp_err_t kiro_passport_network_init(void);
void kiro_passport_network_get_config(kiro_passport_network_config_t *config);

/*
 * Stores an enrollment-issued device credential. Persistence is refused unless
 * NVS encryption is enabled; deployment/API tokens are never accepted here.
 */
esp_err_t kiro_passport_network_configure(const char *relay_url, const char *credential);
esp_err_t kiro_passport_network_clear_configuration(void);

kiro_passport_network_state_t kiro_passport_network_get_state(void);
const char *kiro_passport_network_state_name(kiro_passport_network_state_t state);
