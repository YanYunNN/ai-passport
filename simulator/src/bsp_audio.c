/* simulator/src/bsp_audio.c
 * 虚拟音频（P0 桩）：结构存在但 codec 不可用。
 *
 * 行为对齐固件的降级路径：
 *  - bsp_audio_init() 返回 OK（设备"存在"）
 *  - bsp_audio_set_format() 返回 ESP_ERR_NOT_SUPPORTED
 *    → demo_audio 页显示 format failed；game_audio 静音运行
 *  - write/read 返回 ESP_ERR_NOT_SUPPORTED
 *
 * P1 目标：SDL 音频输出播放 PCM（bsp_audio_write 真实现），麦克风再议。
 */
#include "bsp_audio.h"

esp_err_t bsp_audio_init(void)
{
    return ESP_OK;
}

esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch)
{
    (void)hz;
    (void)bits;
    (void)ch;
    return ESP_ERR_NOT_SUPPORTED; /* P0 未模拟 codec */
}

esp_err_t bsp_audio_close(void)
{
    return ESP_OK;
}

esp_err_t bsp_audio_write(const void *pcm, size_t bytes)
{
    (void)pcm;
    (void)bytes;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_audio_read(void *pcm, size_t bytes)
{
    (void)pcm;
    (void)bytes;
    return ESP_ERR_NOT_SUPPORTED;
}

void bsp_audio_set_volume(uint8_t percent)
{
    (void)percent;
}
