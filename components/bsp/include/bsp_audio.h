// components/bsp/include/bsp_audio.h
// ES8311 audio codec: I2C control and full-duplex I2S transport.
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/* Initializes codec handles and I2S channels. The channels remain disabled at idle. */
esp_err_t bsp_audio_init(void);

/* Opens the codec and starts I2S for the requested active record/playback window. */
esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch);

/* Closes the codec and disables I2S immediately, releasing its APB PM lock. */
esp_err_t bsp_audio_close(void);

/* Playback / recording. bytes is a byte count. Both require an open format. */
esp_err_t bsp_audio_write(const void *pcm, size_t bytes);
esp_err_t bsp_audio_read(void *pcm, size_t bytes);

/* Output volume 0..100 (%); valid while audio is open. */
void bsp_audio_set_volume(uint8_t percent);
