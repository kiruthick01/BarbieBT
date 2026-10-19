/* Stage 5: full call device.
 *   incoming call -> local ringtone -> answer -> voice link + audio bridge -> hang up
 *
 * Audio path (HFP over HCI, narrowband CVSD, 8 kHz 16-bit mono, 60-byte frames = 30 samples):
 *   phone -> incoming_cb -> rx ringbuffer -> audio_task -> upsample x2 -> I2S TX -> MAX98357A
 *   INMP441 -> I2S RX -> decimate /2 -> mic ringbuffer -> outgoing_cb -> phone
 * I2S runs at 16 kHz; one audio_task iteration = 60 I2S frames = one 30-sample SCO frame.
 *
 * Serial keys: a answer, h hang up, 1/2/3 pick ringtone, r ring preview on/off, c/d audio on/off.
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_hf_client_api.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "stage5";

#define SCO_SAMPLES 30                /* 8 kHz samples per SCO frame */
#define SCO_BYTES (SCO_SAMPLES * 2)
#define I2S_FRAMES (SCO_SAMPLES * 2)  /* 16 kHz frames per iteration */
#define RB_SIZE 1440                  /* multiple of 60 and 120: frames never wrap mid-item */
#define RX_PRIME_BYTES 480            /* jitter cushion: ~30 ms before playback starts */

typedef enum { ST_IDLE, ST_RINGING, ST_IN_CALL } call_state_t;

static volatile call_state_t s_state = ST_IDLE;
static volatile bool s_call_active;
static volatile bool s_audio_up;
static volatile uint16_t s_sco_handle;
static volatile bool s_preview;
static volatile int s_ringtone;
static esp_bd_addr_t s_peer;
static bool s_slc;

static RingbufHandle_t s_rx_rb;  /* phone -> speaker */
static RingbufHandle_t s_mic_rb; /* mic -> phone */

/* ---- ringtones: PLACEHOLDER melodies (not the Tamil songs yet). {Hz, ms}, 0 Hz = rest ---- */
typedef struct { uint16_t hz; uint16_t ms; } note_t;

static const note_t RING_A[] = {
    { 784, 120 }, { 988, 120 }, { 1175, 120 }, { 988, 120 }, { 784, 240 }, { 0, 400 },
};
static const note_t RING_B[] = {
    { 523, 150 }, { 659, 150 }, { 784, 150 }, { 1047, 300 }, { 784, 150 }, { 1047, 300 }, { 0, 500 },
};
static const note_t RING_C[] = {
    { 880, 100 }, { 0, 50 }, { 880, 100 }, { 0, 50 }, { 880, 100 }, { 0, 50 }, { 1175, 300 }, { 0, 600 },
};
static const struct { const note_t *n; size_t len; } RINGS[] = {
    { RING_A, sizeof(RING_A) / sizeof(RING_A[0]) },
    { RING_B, sizeof(RING_B) / sizeof(RING_B[0]) },
    { RING_C, sizeof(RING_C) / sizeof(RING_C[0]) },
};

/* debug counters, printed every ~2 s during a call */
static volatile uint32_t s_rx_calls, s_rx_bytes, s_rx_len_last, s_rx_drop, s_rx_in_call;
static volatile int s_rx_peak;
static volatile uint32_t s_out_calls, s_out_bytes, s_out_empty, s_out_len_last;
static volatile int s_mic_peak;
static volatile uint32_t s_underruns;

/* Read exactly n bytes or nothing useful: a byte ringbuffer returns a short read at its end. */
static bool rb_read_exact(RingbufHandle_t rb, uint8_t *dst, size_t n)
{
    if (RB_SIZE - xRingbufferGetCurFreeSize(rb) < n) {
        return false;
    }
    size_t done = 0;
    while (done < n) {
        size_t got = 0;
        void *p = xRingbufferReceiveUpTo(rb, &got, 0, n - done);
        if (!p) {
            return false;
        }
        memcpy(dst + done, p, got);
        vRingbufferReturnItem(rb, p);
        done += got;
    }
    return true;
}

/* ---- HFP data callbacks (run in the BT task: keep them short and non-blocking) ---- */
static void incoming_cb(const uint8_t *buf, uint32_t len)
{
    s_rx_calls++;
    s_rx_bytes += len;
    s_rx_len_last = len;
    const int16_t *p = (const int16_t *)buf;
    for (uint32_t i = 0; i < len / 2; i++) {
        int a = p[i] < 0 ? -p[i] : p[i];
        if (a > s_rx_peak) {
            s_rx_peak = a;
        }
    }
    if (s_state == ST_IN_CALL) {
        s_rx_in_call++;
        if (!xRingbufferSend(s_rx_rb, buf, len, 0)) { /* drops newest on overflow: bounds latency */
            s_rx_drop++;
        }
    }
}

static uint32_t outgoing_cb(uint8_t *buf, uint32_t len)
{
    s_out_calls++;
    s_out_len_last = len;
    if (!s_audio_up) {
        return 0;
    }
    /* the stack wants a full frame or nothing: a short read is discarded */
    if (!rb_read_exact(s_mic_rb, buf, len)) {
        s_out_empty++;
        return 0;
    }
    s_out_bytes += len;
    return len;
}

/* ---- state helpers ---- */
static void set_state(call_state_t st)
{
    if (st == s_state) {
        return;
    }
    s_state = st;
    ESP_LOGI(TAG, "state -> %s", st == ST_IDLE ? "IDLE" : st == ST_RINGING ? "RINGING" : "IN_CALL");
}

/* ---- audio DSP ---- */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } biquad_t;

/* 2nd-order Butterworth high-pass */
static void biquad_hp(biquad_t *f, float fc, float fs)
{
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float alpha = sinf(w0) / (2.0f * 0.7071f);
    float c = cosf(w0);
    float a0 = 1.0f + alpha;
    f->b0 = (1.0f + c) / 2.0f / a0;
    f->b1 = -(1.0f + c) / a0;
    f->b2 = f->b0;
    f->a1 = -2.0f * c / a0;
    f->a2 = (1.0f - alpha) / a0;
    f->z1 = f->z2 = 0;
}

static float biquad_run(biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

typedef struct { float env, gain; } agc_t;

/* Peak-following AGC. Below `gate` the gain is held (and relaxes to 4x) so hiss is not boosted.
 * `freeze` keeps the gain as is, used while the speaker is playing so mic echo is not amplified. */
static float agc_run(agc_t *a, float x, float target, float max_gain, float gate, bool freeze)
{
    float ax = fabsf(x);
    a->env += (ax > a->env ? 0.1f : 0.002f) * (ax - a->env);
    if (freeze) {
        return x * a->gain;
    }
    if (a->env > gate) {
        float want = target / a->env;
        if (want > max_gain) {
            want = max_gain;
        }
        if (want < 0.3f) {
            want = 0.3f;
        }
        a->gain += (want > a->gain ? 0.0015f : 0.02f) * (want - a->gain);
    } else if (a->gain > 4.0f) {
        a->gain += 0.0005f * (4.0f - a->gain);
    }
    return x * a->gain;
}

static float soft_limit(float x)
{
    return 32767.0f * tanhf(x * (1.0f / 32767.0f));
}

static int16_t to_i16(float x)
{
    return (int16_t)(x > 32767.0f ? 32767.0f : x < -32768.0f ? -32768.0f : x);
}

/* INMP441 24-bit (left-justified in 32) -> float in 16-bit range, no clipping yet */
static float mic_f(int32_t raw)
{
    return (float)(raw >> 8) * (1.0f / 256.0f);
}

/* ~2 s capture of both directions (8 kHz int16) for offline analysis; key 'x' dumps it as hex */
#define CAP_N 16200 /* 540 frames of 30 samples */
static int16_t *s_cap_rx; /* as received from the phone (raw) */
static int16_t *s_cap_tx; /* as sent to the phone (after DSP) */
static volatile int s_cap_idx;
static volatile bool s_cap_freeze;

/* ---- audio task ---- */
static void audio_task(void *arg)
{
    static int32_t in[I2S_FRAMES * 2];
    static int32_t out[I2S_FRAMES * 2];
    const float ring_amp = CONFIG_BARBIE_RING_VOLUME_PCT / 100.0f;
    const float call_vol = 65536.0f * CONFIG_BARBIE_CALL_VOLUME_PCT / 100.0f;

    float phase = 0;
    size_t note_idx = 0, note_pos = 0;
    int cur_ring = -1;

    biquad_t rx_hp, tx_hp;
    biquad_hp(&rx_hp, 300.0f, 8000.0f);
    biquad_hp(&tx_hp, 200.0f, 8000.0f);
    agc_t rx_agc = { 0, 1.0f }, tx_agc = { 0, 1.0f };
    float duck = 1.0f;
    int far_hold = 0; /* mic samples left in which the speaker still counts as playing */
    float w[7] = { 0 };                   /* 16 kHz mic history for the 7-tap halfband decimator */
    float u0 = 0, u1 = 0, u2 = 0, u3 = 0; /* 8 kHz speaker history for the cubic upsampler */
    int stat_iter = 0;
    bool primed = false;

    for (;;) {
        call_state_t st = s_state;
        bool ringing = (st == ST_RINGING) || s_preview;

        if (ringing) {
            if (cur_ring != s_ringtone) {
                cur_ring = s_ringtone;
                note_idx = 0;
                note_pos = 0;
            }
            const note_t *notes = RINGS[cur_ring].n;
            for (int i = 0; i < I2S_FRAMES; i++) {
                const note_t *n = &notes[note_idx];
                int32_t s = 0;
                if (n->hz) {
                    /* square wave: toy-phone character */
                    s = (phase < (float)M_PI ? 1 : -1) * (int32_t)(ring_amp * 2147483647.0f * 0.5f);
                    phase += 2.0f * (float)M_PI * n->hz / AUDIO_SAMPLE_RATE;
                    if (phase >= 2.0f * (float)M_PI) {
                        phase -= 2.0f * (float)M_PI;
                    }
                }
                out[2 * i] = s;
                out[2 * i + 1] = s;
                if (++note_pos >= (size_t)n->ms * AUDIO_SAMPLE_RATE / 1000) {
                    note_pos = 0;
                    if (++note_idx >= RINGS[cur_ring].len) {
                        note_idx = 0;
                    }
                }
            }
            audio_io_write(out, I2S_FRAMES, 200);
        } else if (st == ST_IN_CALL && !s_audio_up) {
            /* voice link not established yet: do not touch the BT data path */
            primed = false;
            vTaskDelay(pdMS_TO_TICKS(10));
        } else if (st == ST_IN_CALL) {
            size_t got = 0;
            if (audio_io_read(in, I2S_FRAMES, &got, 100) != ESP_OK || got < I2S_FRAMES) {
                continue;
            }

            /* ---- mic -> phone: 7-tap halfband /2 (16k -> 8k), HP, AGC, echo duck, limiter ---- */
            int16_t mic[SCO_SAMPLES];
            int k = 0;
            for (int i = 0; i < I2S_FRAMES; i++) {
                memmove(w, w + 1, 6 * sizeof(float));
                w[6] = mic_f(in[2 * i]);
                if (i & 1) {
                    float x = (-w[0] + 9.0f * w[2] + 16.0f * w[3] + 9.0f * w[4] - w[6]) * (1.0f / 32.0f);
                    x = biquad_run(&tx_hp, x);
                    /* voice switch: judge by what the speaker plays (env x gain), hold ~120 ms for the
                     * I2S/DMA delay, then duck the mic hard so the caller does not hear themselves back */
                    if (rx_agc.env * rx_agc.gain > 900.0f) {
                        far_hold = 960;
                    } else if (far_hold > 0) {
                        far_hold--;
                    }
                    bool far = far_hold > 0;
                    x = agc_run(&tx_agc, x, 10000.0f, 16.0f, 150.0f, far);
                    float tgt = far ? 0.12f : 1.0f;
                    duck += (tgt < duck ? 0.03f : 0.002f) * (tgt - duck);
                    mic[k] = to_i16(soft_limit(x * duck));
                    int am = mic[k] < 0 ? -mic[k] : mic[k];
                    if (am > s_mic_peak) {
                        s_mic_peak = am;
                    }
                    k++;
                }
            }
            xRingbufferSend(s_mic_rb, mic, SCO_BYTES, 0);
            esp_hf_client_outgoing_data_ready();

            /* ---- phone -> speaker: pop one SCO frame (zeros on underrun) ---- */
            int16_t spk[SCO_SAMPLES] = { 0 };
            if (!primed && RB_SIZE - xRingbufferGetCurFreeSize(s_rx_rb) >= RX_PRIME_BYTES) {
                primed = true;
            }
            if (primed && !rb_read_exact(s_rx_rb, (uint8_t *)spk, SCO_BYTES)) {
                memset(spk, 0, sizeof(spk));
                s_underruns++;
                primed = false; /* refill the cushion before playing again */
            }

            if (s_cap_rx && s_cap_tx && !s_cap_freeze) {
                memcpy(&s_cap_rx[s_cap_idx], spk, SCO_BYTES);
                memcpy(&s_cap_tx[s_cap_idx], mic, SCO_BYTES);
                s_cap_idx += SCO_SAMPLES;
                if (s_cap_idx >= CAP_N) {
                    s_cap_idx = 0;
                }
            }

            /* HP 300 Hz -> AGC -> limiter -> cubic x2 upsample -> I2S */
            for (int j = 0; j < SCO_SAMPLES; j++) {
                float x = biquad_run(&rx_hp, (float)spk[j]);
                float y = soft_limit(agc_run(&rx_agc, x, 14000.0f, 6.0f, 120.0f, false));
                u0 = u1;
                u1 = u2;
                u2 = u3;
                u3 = y;
                float mid = (-u0 + 9.0f * u1 + 9.0f * u2 - u3) * (1.0f / 16.0f);
                out[4 * j] = out[4 * j + 1] = (int32_t)(to_i16(u1) * call_vol);
                out[4 * j + 2] = out[4 * j + 3] = (int32_t)(to_i16(mid) * call_vol);
            }
            audio_io_write(out, I2S_FRAMES, 200);

            if (++stat_iter >= 500) { /* ~1.9 s */
                stat_iter = 0;
                ESP_LOGI(TAG, "STATS rx: calls=%" PRIu32 " drop=%" PRIu32 " peak=%d env=%.0f gain=%.2f | underruns=%" PRIu32
                         " | tx: sent=%" PRIu32 " empty=%" PRIu32 " peak=%d env=%.0f gain=%.2f duck=%.2f",
                         s_rx_calls, s_rx_drop, s_rx_peak, rx_agc.env, rx_agc.gain, s_underruns,
                         s_out_bytes, s_out_empty, s_mic_peak, tx_agc.env, tx_agc.gain, duck);
                s_rx_peak = 0;
                s_mic_peak = 0;
                esp_hf_client_pkt_stat_nums_get(s_sco_handle); /* result arrives as a PKT log line */
            }
        } else {
            cur_ring = -1;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* ---- BT callbacks ---- */
static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "paired with %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "auth failed, status %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "pairing code %" PRIu32 " - accepting", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    default:
        break;
    }
}

static void hf_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    switch (event) {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "connection state %d", param->conn_stat.state);
        if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            memcpy(s_peer, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            s_slc = true;
        } else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            s_slc = false;
            s_call_active = false;
            set_state(ST_IDLE);
        }
        break;
    case ESP_HF_CLIENT_PKT_STAT_NUMS_GET_EVT:
        ESP_LOGI(TAG, "PKT rx: total=%" PRIu32 " ok=%" PRIu32 " err=%" PRIu32 " none=%" PRIu32 " lost=%" PRIu32
                 " | tx: total=%" PRIu32 " discarded=%" PRIu32,
                 param->pkt_nums.rx_total, param->pkt_nums.rx_correct, param->pkt_nums.rx_err,
                 param->pkt_nums.rx_none, param->pkt_nums.rx_lost,
                 param->pkt_nums.tx_total, param->pkt_nums.tx_discarded);
        break;
    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        s_sco_handle = param->audio_stat.sync_conn_handle;
        s_audio_up =param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ||
                     param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC;
        ESP_LOGI(TAG, "audio (SCO) state %d%s", param->audio_stat.state,
                 param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC
                     ? "  (mSBC! bridge expects CVSD)" : "");
        break;
    case ESP_HF_CLIENT_CIND_CALL_EVT:
        ESP_LOGI(TAG, "CALL status %d", param->call.status);
        s_call_active = param->call.status == 1;
        if (s_call_active) {
            set_state(ST_IN_CALL);
            if (!s_audio_up && s_slc) {
                esp_hf_client_connect_audio(s_peer);
            }
        } else {
            set_state(ST_IDLE);
            s_audio_up = false;
        }
        break;
    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        ESP_LOGI(TAG, "CALL_SETUP status %d", param->call_setup.status);
        if (param->call_setup.status == 1 && !s_call_active) {
            set_state(ST_RINGING);
        } else if (param->call_setup.status == 0 && s_state == ST_RINGING) {
            set_state(ST_IDLE); /* caller gave up or call rejected */
        }
        break;
    default:
        break;
    }
}

/* ---- console ---- */
static void console_task(void *arg)
{
    uint8_t c;
    for (;;) {
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        switch (c) {
        case '1': case '2': case '3':
            s_ringtone = c - '1';
            ESP_LOGI(TAG, "ringtone %d", s_ringtone + 1);
            break;
        case 'r':
            s_preview = !s_preview;
            ESP_LOGI(TAG, "ring preview %s", s_preview ? "on" : "off");
            break;
        case 'a':
            ESP_LOGI(TAG, "answer");
            esp_hf_client_answer_call();
            break;
        case 'h':
            ESP_LOGI(TAG, "hang up");
            esp_hf_client_reject_call();
            break;
        case 'x': /* dump the last ~2 s of both audio streams as hex for offline analysis */
            if (!s_cap_rx || !s_cap_tx) {
                ESP_LOGW(TAG, "no capture buffers");
                break;
            }
            s_cap_freeze = true;
            vTaskDelay(pdMS_TO_TICKS(100));
            printf("CAPBEGIN n=%d idx=%d rate=8000\n", CAP_N, s_cap_idx);
            for (int s = 0; s < 2; s++) {
                const int16_t *b = s ? s_cap_tx : s_cap_rx;
                for (int i = 0; i < CAP_N; i += 30) {
                    printf(s ? "CAPTX " : "CAPRX ");
                    for (int j = 0; j < 30; j++) {
                        printf("%04x", (unsigned)(uint16_t)b[(s_cap_idx + i + j) % CAP_N]);
                    }
                    printf("\n");
                }
            }
            printf("CAPEND\n");
            s_cap_freeze = false;
            break;
        case 'c':
            esp_hf_client_connect_audio(s_peer);
            break;
        case 'd':
            esp_hf_client_disconnect_audio(s_peer);
            break;
        default:
            break;
        }
    }
}

void stage5_call_run(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    audio_io_init();
    s_rx_rb = xRingbufferCreate(RB_SIZE, RINGBUF_TYPE_BYTEBUF);
    s_mic_rb = xRingbufferCreate(RB_SIZE, RINGBUF_TYPE_BYTEBUF);
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 10, NULL, 1);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_hf_client_register_callback(hf_cb));
    ESP_ERROR_CHECK(esp_hf_client_init());
    ESP_ERROR_CHECK(esp_hf_client_register_data_callback(incoming_cb, outgoing_cb));

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_pin_code_t pin = { '0', '0', '0', '0' };
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);
    esp_bt_gap_set_device_name("BarbiePhone");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    xTaskCreate(console_task, "console", 3072, NULL, 5, NULL);

    s_cap_rx = heap_caps_malloc(CAP_N * sizeof(int16_t), MALLOC_CAP_8BIT);
    s_cap_tx = heap_caps_malloc(CAP_N * sizeof(int16_t), MALLOC_CAP_8BIT);
    if (s_cap_rx && s_cap_tx) {
        memset(s_cap_rx, 0, CAP_N * sizeof(int16_t));
        memset(s_cap_tx, 0, CAP_N * sizeof(int16_t));
    } else {
        free(s_cap_rx);
        free(s_cap_tx);
        s_cap_rx = s_cap_tx = NULL;
        ESP_LOGW(TAG, "capture buffers not allocated");
    }
    ESP_LOGI(TAG, "free heap %" PRIu32, esp_get_free_heap_size());
    ESP_LOGI(TAG, "ready. keys: a answer, h hangup, 1/2/3 ringtone, r ring preview, c/d audio, x dump audio");
    vTaskDelay(portMAX_DELAY);
}
