/* simulator/src/stubs_wifi.c
 * Wi-Fi 管理 + 多配置档案 的宿主桩（P0 无真实射频）。
 *
 * wifi_nvs：内存版档案存储（单次运行内有效），让"设置 → 网络"页可操作。
 * wifi_manager：跟踪开关/节能状态；连接状态恒为 UNAVAILABLE（P0 无网络）。
 */
#include "wifi_manager.h"
#include "wifi_nvs.h"

#include <string.h>

/* ==================== wifi_nvs（内存版） ==================== */
#define SIM_PROFILES MAX_WIFI_PROFILES

static wifi_profile_t s_profiles[SIM_PROFILES];
static size_t s_count;
static char s_active_ssid[32];

static int find_profile(const char *ssid)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_profiles[i].ssid, ssid) == 0) return (int)i;
    }
    return -1;
}

esp_err_t wifi_nvs_save_profile(const wifi_profile_t *profile)
{
    if (!profile || !profile->ssid[0]) return ESP_ERR_INVALID_ARG;

    int idx = find_profile(profile->ssid);
    if (idx >= 0) {
        s_profiles[idx] = *profile;
        return ESP_OK;
    }
    if (s_count < SIM_PROFILES) {
        s_profiles[s_count++] = *profile;
        return ESP_OK;
    }
    /* 满：淘汰最不常用（priority 最大）的条目，保留手动选中的档案 */
    int evict = 0;
    for (size_t i = 1; i < s_count; i++) {
        if (s_profiles[i].priority > s_profiles[evict].priority) evict = (int)i;
    }
    if (strcmp(s_profiles[evict].ssid, s_active_ssid) == 0) return ESP_ERR_NO_MEM;
    s_profiles[evict] = *profile;
    return ESP_OK;
}

esp_err_t wifi_nvs_remove_profile(const char *ssid)
{
    int idx = find_profile(ssid);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    for (size_t i = (size_t)idx; i + 1 < s_count; i++) {
        s_profiles[i] = s_profiles[i + 1];
    }
    s_count--;
    if (strcmp(s_active_ssid, ssid) == 0) s_active_ssid[0] = '\0';
    return ESP_OK;
}

esp_err_t wifi_nvs_load_profile(const char *ssid, wifi_profile_t *out_profile)
{
    int idx = find_profile(ssid);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    if (out_profile) *out_profile = s_profiles[idx];
    return ESP_OK;
}

size_t wifi_nvs_get_all_profiles(wifi_profile_t *out_array, size_t max)
{
    size_t n = s_count < max ? s_count : max;
    for (size_t i = 0; i < n; i++) out_array[i] = s_profiles[i];
    return n;
}

size_t wifi_nvs_count_profiles(void)
{
    return s_count;
}

esp_err_t wifi_nvs_set_active_ssid(const char *ssid)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    strncpy(s_active_ssid, ssid, sizeof(s_active_ssid) - 1);
    return ESP_OK;
}

esp_err_t wifi_nvs_get_active_ssid(char *out_ssid, size_t size)
{
    if (!s_active_ssid[0]) return ESP_ERR_NOT_FOUND;
    if (out_ssid && size > 0) {
        strncpy(out_ssid, s_active_ssid, size - 1);
        out_ssid[size - 1] = '\0';
    }
    return ESP_OK;
}

/* ==================== wifi_manager ==================== */
static bool s_enabled = true;
static bool s_power_save = true;

esp_err_t wifi_manager_add_profile(const wifi_profile_t *profile)
{
    return wifi_nvs_save_profile(profile);
}

esp_err_t wifi_manager_remove_profile(const char *ssid)
{
    return wifi_nvs_remove_profile(ssid);
}

esp_err_t wifi_manager_set_active_profile(const char *ssid)
{
    return wifi_nvs_set_active_ssid(ssid);
}

esp_err_t wifi_manager_select_best_profile(void)
{
    return ESP_OK; /* P0：无扫描 */
}

esp_err_t wifi_manager_set_enabled(bool enabled)
{
    s_enabled = enabled;
    return ESP_OK;
}

bool wifi_manager_is_enabled(void)
{
    return s_enabled;
}

esp_err_t wifi_manager_set_power_save(bool enabled)
{
    s_power_save = enabled;
    return ESP_OK;
}

bool wifi_manager_is_power_save_enabled(void)
{
    return s_power_save;
}

esp_err_t wifi_manager_init(void)
{
    return ESP_OK;
}

wifi_manager_state_t wifi_manager_get_state(void)
{
    /* P0 无网络：恒为 UNAVAILABLE，状态栏 Wi-Fi 图标保持灰色 */
    return WIFI_MANAGER_UNAVAILABLE;
}

esp_err_t wifi_manager_get_connected_ssid(char *ssid, size_t size)
{
    (void)ssid;
    (void)size;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_manager_start_provisioning(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void wifi_manager_stop_provisioning(void)
{
}

const char *wifi_manager_get_provisioning_ssid(void)
{
    return "";
}

const char *wifi_manager_get_provisioning_password(void)
{
    return "";
}
