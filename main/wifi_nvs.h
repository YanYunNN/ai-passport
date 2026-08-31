#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// Maximum number of stored Wi-Fi profiles (device memory limitation).
#define MAX_WIFI_PROFILES 5

// Inner/authentication flavour of a WPA2-Enterprise profile. The value is
// stored in wifi_profile_t.eap_method; the field defaults to PEAP so profiles
// written by older firmware (field zeroed) keep their former behaviour.
// Both methods here are "username + password" and share the same credential
// fields; only the outer EAP handshake differs. EAP-TLS is not selectable
// because it requires a client certificate, which the provisioning flow does
// not collect.
#define WIFI_EAP_PEAP 0
#define WIFI_EAP_TTLS 1

// A saved network credential pair. priority 0 = most preferred.
// enterprise != 0 means WPA2-Enterprise: password holds the EAP password,
// identity the login name and eap_method selects PEAP or TTLS.
// For PEAP the identity is applied to both the outer EAP identity and the
// inner MSCHAPv2 username; for TTLS the inner phase-2 username also uses it.
// For WPA2-PSK networks identity stays empty and eap_method is irrelevant.
typedef struct {
    char ssid[32];
    char password[64];
    char identity[64];
    uint8_t priority;
    uint8_t enterprise;
    uint8_t eap_method;
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
