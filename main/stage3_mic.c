/* Stage 3: INMP441 level meter. Prints peak/RMS in dBFS + bar every ~200 ms.
 * INMP441 sends 24-bit data MSB-first in a 32-bit slot, so >>8 gives signed 24-bit.
 * Expect ~ -60 dBFS quiet room, ~ -30 .. -10 dBFS speaking close. Constant 0 or
 * pegged value = wiring problem (L/R pin, SD pin, 3.3 V supply). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "esp_log.h"

static const char *TAG = "stage3";

#define CHUNK 256
#define FULL_SCALE 8388608.0f /* 2^23 */

void stage3_mic_run(void)
{
    static int32_t buf[CHUNK * 2];
    int64_t sum_sq = 0;
    int32_t peak = 0;
    size_t count = 0;
    const size_t report_every = AUDIO_SAMPLE_RATE / 5;

    ESP_LOGI(TAG, "speak or clap near the mic");
    for (;;) {
        size_t got = 0;
        if (audio_io_read(buf, CHUNK, &got, 1000) != ESP_OK) {
            ESP_LOGW(TAG, "read timeout - no clock? check I2S wiring");
            continue;
        }
        for (size_t i = 0; i < got; i++) {
            int32_t s = buf[2 * i] >> 8; /* left slot (L/R pin to GND) */
            int32_t a = s < 0 ? -s : s;
            if (a > peak) {
                peak = a;
            }
            sum_sq += (int64_t)s * s;
        }
        count += got;
        if (count >= report_every) {
            float rms = sqrtf((float)sum_sq / (float)count);
            float rms_db = 20.0f * log10f((rms + 1.0f) / FULL_SCALE);
            float peak_db = 20.0f * log10f(((float)peak + 1.0f) / FULL_SCALE);
            int bars = (int)((rms_db + 80.0f) / 2.0f);
            if (bars < 0) {
                bars = 0;
            }
            if (bars > 40) {
                bars = 40;
            }
            char bar[41];
            memset(bar, '#', bars);
            bar[bars] = '\0';
            printf("rms %6.1f dBFS  peak %6.1f dBFS  |%s\n", rms_db, peak_db, bar);
            sum_sq = 0;
            peak = 0;
            count = 0;
        }
    }
}
