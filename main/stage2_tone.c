/* Stage 2: sine tone to MAX98357A. 2 s tone, 1 s silence, repeat.
 * Start with low CONFIG_BARBIE_TONE_VOLUME_PCT. Salvaged phone speaker is rated ~0.5-1 W. */
#include <math.h>

#include "audio_io.h"
#include "esp_log.h"

static const char *TAG = "stage2";

#define CHUNK 256

static void play(float hz, float amp, int seconds)
{
    static int32_t buf[CHUNK * 2];
    static float phase;
    const float step = 2.0f * (float)M_PI * hz / AUDIO_SAMPLE_RATE;
    const int chunks = seconds * AUDIO_SAMPLE_RATE / CHUNK;

    for (int c = 0; c < chunks; c++) {
        for (int i = 0; i < CHUNK; i++) {
            int32_t s = (int32_t)(sinf(phase) * amp * 2147483647.0f);
            buf[2 * i] = s;
            buf[2 * i + 1] = s;
            phase += step;
            if (phase > 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }
        audio_io_write(buf, CHUNK, 1000);
    }
}

void stage2_tone_run(void)
{
    const float amp = CONFIG_BARBIE_TONE_VOLUME_PCT / 100.0f;
    ESP_LOGI(TAG, "tone %d Hz at %d%% volume", CONFIG_BARBIE_TONE_HZ, CONFIG_BARBIE_TONE_VOLUME_PCT);
    for (;;) {
        play((float)CONFIG_BARBIE_TONE_HZ, amp, 2);
        play(0, 0, 1);
    }
}
