#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// Maximum number of stored Wi-Fi profiles (device memory limitation).
#define MAX_WIFI_PROFILES 5

// A saved network credential pair. priority 0 = most preferred.
typedef struct {
    char ssid[32];
    char password[64];
    uint8_t priority;
} wifi_profile_t;

// Namespace used for storing Wi-Fi profiles.
#define WIFI_NVS_NAMESPACE "wifi_profiles"

// Add or update a profile (matched by SSID). New profiles get the highest
// priority; when the store is full the least preferred entry is evicted,
// keeping the manually selected (active) profile when possible.
esp_err_t wifi_nvs_save_profile(const wifi_profile_t *profile);

// Remove a stored profile by SSID. Also clears the active selection if it
// pointed at this profile.
esp_err_t wifi_nvs_remove_profile(const char *ssid);

// Load a profile by SSID. Returns ESP_ERR_NOT_FOUND if absent.
esp_err_t wifi_nvs_load_profile(const char *ssid, wifi_profile_t *out_profile);

// Copy up to max stored profiles into out_array. Returns the number stored.
size_t wifi_nvs_get_all_profiles(wifi_profile_t *out_array, size_t max);

// Number of currently stored profiles.
size_t wifi_nvs_count_profiles(void);

// Persist the SSID the user manually selected (used as scan tie-breaker).
esp_err_t wifi_nvs_set_active_ssid(const char *ssid);

// Retrieve the manually selected SSID; ESP_ERR_NOT_FOUND if none set.
esp_err_t wifi_nvs_get_active_ssid(char *out_ssid, size_t size);
