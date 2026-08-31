// main/game_audio.c - 游戏音效合成引擎
#include "game_audio.h"
#include "bsp_audio.h"
#include "app_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "game_audio";

#define SAMPLE_RATE     16000
#define CHUNK_SAMPLES   256

static QueueHandle_t s_sfx_queue = NULL;
static TaskHandle_t s_audio_task = NULL;
static bool s_audio_open = false;

static esp_err_t ensure_audio_open(void)
{
    if (!s_audio_open) {
        bsp_audio_init();
        esp_err_t err = bsp_audio_set_format(SAMPLE_RATE, 16, 1);
        if (err == ESP_OK) {
            uint8_t vol = app_settings_get_volume_percent();
            bsp_audio_set_volume(vol);
            s_audio_open = true;
        } else {
            ESP_LOGW(TAG, "Failed to open audio: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static void play_tone_segment(float start_hz, float end_hz, int duration_ms, float max_vol, bool decay)
{
    if (ensure_audio_open() != ESP_OK) return;

    int total_samples = SAMPLE_RATE * duration_ms / 1000;
    if (total_samples <= 0) return;

    int16_t buf[CHUNK_SAMPLES];
    float phase = 0.0f;

    for (int sample_idx = 0; sample_idx < total_samples; ) {
        int chunk = (total_samples - sample_idx < CHUNK_SAMPLES) ? (total_samples - sample_idx) : CHUNK_SAMPLES;
        for (int i = 0; i < chunk; i++) {
            float progress = (float)(sample_idx + i) / (float)total_samples;
            float current_hz = start_hz + (end_hz - start_hz) * progress;
            float vol = max_vol;
            if (decay) {
                vol *= (1.0f - progress * progress); // smooth quadratic decay
            }
            float phase_inc = (2.0f * (float)M_PI * current_hz) / (float)SAMPLE_RATE;
            phase += phase_inc;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;

            // Sine wave with soft harmonic (boosted to 18000.0f for clear loud output)
            float val = sinf(phase) + 0.2f * sinf(2.0f * phase);
            buf[i] = (int16_t)(val * vol * 18000.0f);
        }
        bsp_audio_write(buf, (size_t)chunk * sizeof(int16_t));
        sample_idx += chunk;
    }
}

static void synth_sfx(game_sfx_t sfx)
{
    switch (sfx) {
    case GAME_SFX_MOVE:
        play_tone_segment(520.0f, 600.0f, 25, 0.4f, true);
        break;
    case GAME_SFX_DROP:
        play_tone_segment(600.0f, 320.0f, 45, 0.6f, true);
        break;
    case GAME_SFX_SHOOT:
        play_tone_segment(880.0f, 400.0f, 40, 0.7f, true);
        break;
    case GAME_SFX_POP:
        play_tone_segment(650.0f, 1300.0f, 35, 0.8f, true);
        break;
    case GAME_SFX_MERGE_SMALL:
        play_tone_segment(440.0f, 660.0f, 40, 0.8f, true);
        play_tone_segment(660.0f, 880.0f, 50, 0.9f, true);
        break;
    case GAME_SFX_MERGE_MED:
        play_tone_segment(523.0f, 659.0f, 35, 0.8f, true);
        play_tone_segment(659.0f, 784.0f, 40, 0.85f, true);
        play_tone_segment(784.0f, 1046.0f, 60, 0.9f, true);
        break;
    case GAME_SFX_MERGE_BIG:
        play_tone_segment(523.0f, 659.0f, 30, 0.8f, true);
        play_tone_segment(659.0f, 784.0f, 30, 0.85f, true);
        play_tone_segment(784.0f, 1046.0f, 40, 0.9f, true);
        play_tone_segment(1046.0f, 1318.0f, 50, 0.95f, true);
        play_tone_segment(1318.0f, 1568.0f, 80, 1.0f, true);
        break;
    case GAME_SFX_COMBO:
        play_tone_segment(700.0f, 950.0f, 30, 0.8f, true);
        play_tone_segment(950.0f, 1200.0f, 35, 0.9f, true);
        play_tone_segment(1200.0f, 1600.0f, 60, 1.0f, true);
        break;
    case GAME_SFX_OVER:
        play_tone_segment(400.0f, 300.0f, 70, 0.7f, true);
        play_tone_segment(300.0f, 220.0f, 90, 0.7f, true);
        play_tone_segment(220.0f, 140.0f, 120, 0.6f, true);
        break;
    case GAME_SFX_WIN:
        play_tone_segment(523.0f, 659.0f, 40, 0.8f, true);
        play_tone_segment(659.0f, 784.0f, 40, 0.8f, true);
        play_tone_segment(784.0f, 1046.0f, 50, 0.9f, true);
        play_tone_segment(1046.0f, 1318.0f, 90, 1.0f, true);
        break;
    default:
        break;
    }
}

static void audio_worker_task(void *arg)
{
    /* 队列在任务创建时绑定, 不随全局 s_sfx_queue 变化:
     * 模拟器的 vTaskDelete 不终止线程, 旧 worker 会残留; 若每轮都读全局,
     * 重进游戏页后旧 worker 会读到重建的新队列, 与当前 worker 并发处理音效。 */
    QueueHandle_t queue = (QueueHandle_t)arg;
    game_sfx_t sfx;
    int idle_count = 0;

    for (;;) {
        if (xQueueReceive(queue, &sfx, pdMS_TO_TICKS(100)) == pdTRUE) {
            idle_count = 0;
            if (sfx != GAME_SFX_NONE) {
                synth_sfx(sfx);
            }
        } else {
            idle_count++;
            // Close audio codec after 1.5 seconds of silence to conserve power
            if (idle_count > 15 && s_audio_open) {
                bsp_audio_close();
                s_audio_open = false;
            }
        }
    }
}

void game_audio_init(void)
{
    if (!s_sfx_queue) {
        s_sfx_queue = xQueueCreate(8, sizeof(game_sfx_t));
    }
    if (!s_audio_task) {
        xTaskCreate(audio_worker_task, "game_audio", 3072, s_sfx_queue, 4, &s_audio_task);
    }
}

void game_audio_deinit(void)
{
    if (s_audio_task) {
        vTaskDelete(s_audio_task);
        s_audio_task = NULL;
    }
    if (s_sfx_queue) {
        vQueueDelete(s_sfx_queue);
        s_sfx_queue = NULL;
    }
    if (s_audio_open) {
        bsp_audio_close();
        s_audio_open = false;
    }
}

void game_audio_play(game_sfx_t sfx)
{
    if (!s_sfx_queue || !s_audio_task) {
        game_audio_init();
    }
    if (s_sfx_queue) {
        xQueueSend(s_sfx_queue, &sfx, 0); // Non-blocking
    }
}
