#include "wifi_nvs.h"
#include "esp_log.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wifi_nvs";

// Slot keys p0..p4. SSIDs are never used as NVS keys because NVS limits key
// names to 15 characters.
static void slot_key(size_t slot, char *key, size_t size)
{
    snprintf(key, size, "p%u", (unsigned)slot);
}

static bool load_slot(nvs_handle_t handle, size_t slot, wifi_profile_t *out)
{
    char key[8];
    slot_key(slot, key, sizeof(key));
    // Accept blobs smaller than the current struct so profiles written by an
    // older firmware (without the enterprise fields) still load; the missing
    // tail stays zeroed.
    size_t required = sizeof(*out);
    memset(out, 0, sizeof(*out));
    if (nvs_get_blob(handle, key, out, &required) != ESP_OK) return false;
    if (required == 0 || required > sizeof(*out)) return false;
    return out->ssid[0];
}

static esp_err_t store_slot(nvs_handle_t handle, size_t slot,
                            const wifi_profile_t *profile)
{
    char key[8];
    slot_key(slot, key, sizeof(key));
    return nvs_set_blob(handle, key, profile, sizeof(*profile));
}

static esp_err_t erase_slot(nvs_handle_t handle, size_t slot)
{
    char key[8];
    slot_key(slot, key, sizeof(key));
    return nvs_erase_key(handle, key);
}

esp_err_t wifi_nvs_save_profile(const wifi_profile_t *profile)
{
    if (!profile || !profile->ssid[0] || !profile->password[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    char active_ssid[33];
    size_t active_len = sizeof(active_ssid);
    bool has_active = nvs_get_str(handle, "active", active_ssid,
                                  &active_len) == ESP_OK;

    wifi_profile_t entries[MAX_WIFI_PROFILES];
    size_t slots[MAX_WIFI_PROFILES];
    size_t count = 0;
    for (size_t i = 0; i < MAX_WIFI_PROFILES; i++) {
        wifi_profile_t existing;
        if (load_slot(handle, i, &existing)) {
            entries[count] = existing;
            slots[count] = i;
            count++;
        }
    }

    // Updating an existing SSID keeps its priority and only refreshes the
    // credential, so the list order is stable across re-provisioning.
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].ssid, profile->ssid) != 0) continue;
        uint8_t stored_priority = entries[i].priority;
        entries[i] = *profile;
        entries[i].priority = stored_priority;
        err = store_slot(handle, slots[i], &entries[i]);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
        if (err == ESP_OK) ESP_LOGI(TAG, "已更新 Wi-Fi 配置: %s", entries[i].ssid);
        return err;
    }

    // New profile: it becomes the newest (priority 0) and all existing
    // entries are demoted by one. Priorities stay in 0..MAX_WIFI_PROFILES-1.
    int slot = -1;
    bool used[MAX_WIFI_PROFILES] = { false };
    for (size_t i = 0; i < count; i++) used[slots[i]] = true;
    for (size_t i = 0; i < MAX_WIFI_PROFILES; i++) {
        if (!used[i]) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        // Full: evict the least preferred (oldest) entry, preferring to keep
        // the manually selected profile.
        size_t evict = 0;
        for (size_t i = 1; i < count; i++) {
            if (entries[i].priority > entries[evict].priority) evict = i;
        }
        if (has_active && strcmp(entries[evict].ssid, active_ssid) == 0) {
            size_t alt = SIZE_MAX;
            for (size_t i = 0; i < count; i++) {
                if (strcmp(entries[i].ssid, active_ssid) == 0) continue;
                if (alt == SIZE_MAX ||
                    entries[i].priority > entries[alt].priority) {
                    alt = i;
                }
            }
            if (alt != SIZE_MAX) evict = alt;
        }
        slot = (int)slots[evict];
        err = erase_slot(handle, (size_t)slot);
        if (err != ESP_OK) {
            nvs_close(handle);
            return err;
        }
        for (size_t i = 0; i < count; i++) {
            if (i == evict) continue;
            if (entries[i].priority < MAX_WIFI_PROFILES - 1) {
                entries[i].priority++;
            }
            err = store_slot(handle, slots[i], &entries[i]);
            if (err != ESP_OK) {
                nvs_close(handle);
                return err;
            }
        }
    } else {
        for (size_t i = 0; i < count; i++) {
            if (entries[i].priority < MAX_WIFI_PROFILES - 1) {
                entries[i].priority++;
            }
            err = store_slot(handle, slots[i], &entries[i]);
            if (err != ESP_OK) {
                nvs_close(handle);
                return err;
            }
        }
    }

    wifi_profile_t fresh = *profile;
    fresh.priority = 0;
    err = store_slot(handle, (size_t)slot, &fresh);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) ESP_LOGI(TAG, "已保存 Wi-Fi 配置: %s", fresh.ssid);
    return err;
}

esp_err_t wifi_nvs_remove_profile(const char *ssid)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    char active_ssid[33];
    size_t active_len = sizeof(active_ssid);
    bool has_active = nvs_get_str(handle, "active", active_ssid,
                                  &active_len) == ESP_OK;

    for (size_t i = 0; i < MAX_WIFI_PROFILES; i++) {
        wifi_profile_t profile;
        if (!load_slot(handle, i, &profile)) continue;
        if (strcmp(profile.ssid, ssid) != 0) continue;
        err = erase_slot(handle, i);
        if (err == ESP_OK && has_active &&
            strcmp(profile.ssid, active_ssid) == 0) {
            err = nvs_erase_key(handle, "active");
        }
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
        if (err == ESP_OK) ESP_LOGI(TAG, "已删除 Wi-Fi 配置: %s", ssid);
        return err;
    }
    nvs_close(handle);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_nvs_load_profile(const char *ssid, wifi_profile_t *out_profile)
{
    if (!ssid || !out_profile) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    for (size_t i = 0; i < MAX_WIFI_PROFILES; i++) {
        wifi_profile_t profile;
        if (load_slot(handle, i, &profile) &&
            strcmp(profile.ssid, ssid) == 0) {
            *out_profile = profile;
            nvs_close(handle);
            return ESP_OK;
        }
    }
    nvs_close(handle);
    return ESP_ERR_NOT_FOUND;
}

size_t wifi_nvs_get_all_profiles(wifi_profile_t *out_array, size_t max)
{
    if (!out_array || max == 0) return 0;

    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;

    size_t count = 0;
    for (size_t i = 0; i < MAX_WIFI_PROFILES && count < max; i++) {
        if (load_slot(handle, i, &out_array[count])) count++;
    }
    nvs_close(handle);
    return count;
}

size_t wifi_nvs_count_profiles(void)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;

    size_t count = 0;
    for (size_t i = 0; i < MAX_WIFI_PROFILES; i++) {
        wifi_profile_t profile;
        if (load_slot(handle, i, &profile)) count++;
    }
    nvs_close(handle);
    return count;
}

esp_err_t wifi_nvs_set_active_ssid(const char *ssid)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "active", ssid);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_nvs_get_active_ssid(char *out_ssid, size_t size)
{
    if (!out_ssid || size == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_str(handle, "active", out_ssid, &size);
    nvs_close(handle);
    return err;
}
