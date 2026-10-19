#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE 16000

/* Full-duplex I2S0, 32-bit slots, stereo frames (L,R interleaved).
 * TX -> MAX98357A, RX <- INMP441 (L/R pin to GND = left slot). */
esp_err_t audio_io_init(void);

/* Write interleaved stereo int32 frames. n_frames = frame count. */
esp_err_t audio_io_write(const int32_t *frames, size_t n_frames, uint32_t timeout_ms);

/* Read interleaved stereo int32 frames. Returns frames read via out param. */
esp_err_t audio_io_read(int32_t *frames, size_t n_frames, size_t *got_frames, uint32_t timeout_ms);

void stage2_tone_run(void);
void stage3_mic_run(void);
void stage4_hfp_run(void);
void stage5_call_run(void);
