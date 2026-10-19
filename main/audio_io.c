#include "audio_io.h"

#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "audio_io";

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;

esp_err_t audio_io_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 60; /* small DMA queue keeps call latency low */
    chan_cfg.auto_clear = true;  /* TX underrun -> silence, not a repeated buffer */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, &s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_BARBIE_PIN_BCLK,
            .ws = CONFIG_BARBIE_PIN_WS,
            .dout = CONFIG_BARBIE_PIN_DOUT,
            .din = CONFIG_BARBIE_PIN_DIN,
            .invert_flags = { false, false, false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
    ESP_LOGI(TAG, "I2S up: %d Hz, BCLK=%d WS=%d DOUT=%d DIN=%d", AUDIO_SAMPLE_RATE,
             CONFIG_BARBIE_PIN_BCLK, CONFIG_BARBIE_PIN_WS,
             CONFIG_BARBIE_PIN_DOUT, CONFIG_BARBIE_PIN_DIN);
    return ESP_OK;
}

esp_err_t audio_io_write(const int32_t *frames, size_t n_frames, uint32_t timeout_ms)
{
    size_t written = 0;
    return i2s_channel_write(s_tx, frames, n_frames * 2 * sizeof(int32_t), &written, timeout_ms);
}

esp_err_t audio_io_read(int32_t *frames, size_t n_frames, size_t *got_frames, uint32_t timeout_ms)
{
    size_t got = 0;
    esp_err_t err = i2s_channel_read(s_rx, frames, n_frames * 2 * sizeof(int32_t), &got, timeout_ms);
    if (got_frames) {
        *got_frames = got / (2 * sizeof(int32_t));
    }
    return err;
}
