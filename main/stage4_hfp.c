/* Stage 4: Bluetooth Classic HFP client. Logs phone events; serial keys:
 *   a = answer   h = hang up / reject   c = connect audio   d = disconnect audio
 * Pair from the phone (Settings > Bluetooth > "BarbiePhone"), tick "Phone calls",
 * then call the phone from another number.
 * No audio bridge here (stage 6). SCO audio state is only logged. */
#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "stage4";

static esp_bd_addr_t s_peer;
static bool s_connected;

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
    case ESP_BT_GAP_CFM_REQ_EVT: /* SSP numeric comparison: accept */
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
            s_connected = true;
        } else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            s_connected = false;
        }
        break;
    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "audio (SCO) state %d", param->audio_stat.state);
        break;
    case ESP_HF_CLIENT_CIND_CALL_EVT:
        ESP_LOGI(TAG, "CALL status %d (0=none 1=active)", param->call.status);
        break;
    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        ESP_LOGI(TAG, "CALL_SETUP status %d (0=none 1=incoming 2=outgoing 3=alerting)",
                 param->call_setup.status);
        break;
    case ESP_HF_CLIENT_RING_IND_EVT:
        ESP_LOGI(TAG, "RING");
        break;
    case ESP_HF_CLIENT_CLIP_EVT:
        ESP_LOGI(TAG, "caller: %s", param->clip.number);
        break;
    default:
        ESP_LOGD(TAG, "hf event %d", event);
        break;
    }
}

static void console_task(void *arg)
{
    uint8_t c;
    for (;;) {
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (!s_connected && c != '\n' && c != '\r') {
            ESP_LOGW(TAG, "no phone connected yet");
            continue;
        }
        switch (c) {
        case 'a':
            ESP_LOGI(TAG, "answer");
            esp_hf_client_answer_call();
            break;
        case 'h':
            ESP_LOGI(TAG, "hang up");
            esp_hf_client_reject_call();
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

void stage4_hfp_run(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_hf_client_register_callback(hf_cb));
    ESP_ERROR_CHECK(esp_hf_client_init());

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE; /* "just works" pairing */
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_pin_code_t pin = { '0', '0', '0', '0' }; /* legacy fallback */
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);

    esp_bt_gap_set_device_name("BarbiePhone");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    xTaskCreate(console_task, "console", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "discoverable as BarbiePhone. keys: a answer, h hangup, c/d audio on/off");
    vTaskDelay(portMAX_DELAY);
}
