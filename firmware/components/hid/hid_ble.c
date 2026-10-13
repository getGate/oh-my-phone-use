/*
 * Bluetooth Low Energy HID transport (HID over GATT) built on ESP-IDF's esp_hid component
 * and the Bluedroid host. This is the only file that touches Bluetooth HID internals.
 *
 * Responsibilities:
 *   - HID report map (keyboard, report ID 1; mouse, report ID 2)
 *   - BLE GAP: security parameters, advertising data, pairing events
 *   - esp_hidd device lifecycle and connection tracking
 *   - sending input reports
 *
 * Notes for iOS:
 *   - Appearance 0x03C2 (Mouse) is advertised: the generic HID appearance is known to be
 *     ignored by some iOS versions (asterics/esp32_mouse_keyboard issue #52), and the
 *     pointer must be recognised for AssistiveTouch.
 *   - iOS writes the report CCC before the link is encrypted, gets "insufficient
 *     encryption", pairs, and retries. esp_hid marks the CCC WRITE_ENCRYPTED for that.
 *   - Pairing is "Just Works" LE Secure Connections bonding by default; the bond is kept
 *     in NVS so the phone reconnects automatically after a reset.
 *
 * Threading: GAP events arrive on the Bluedroid task, esp_hidd events on the esp_hid event
 * task. State is guarded by a mutex. Reports are sent from the caller's task and block until
 * the stack has queued the notification (esp_hidd_dev_input_set waits for ESP_GATTS_CONF_EVT),
 * which is why hid_device.c never sends from a callback.
 */
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "bt_stack.h"
#include "hid_transport.h"

static const char *TAG = "hid_ble";

/* ---- HID report map ------------------------------------------------------------------- */

static const uint8_t s_report_map[] = {
    /* Keyboard: report ID 1, 8-byte input (modifiers, reserved, 6 keys), 1-byte LED output */
    0x05, 0x01,                     // Usage Page (Generic Desktop)
    0x09, 0x06,                     // Usage (Keyboard)
    0xA1, 0x01,                     // Collection (Application)
    0x85, HID_REPORT_ID_KEYBOARD,   //   Report ID (1)
    0x05, 0x07,                     //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,                     //   Usage Minimum (Left Control)
    0x29, 0xE7,                     //   Usage Maximum (Right GUI)
    0x15, 0x00,                     //   Logical Minimum (0)
    0x25, 0x01,                     //   Logical Maximum (1)
    0x75, 0x01,                     //   Report Size (1)
    0x95, 0x08,                     //   Report Count (8)
    0x81, 0x02,                     //   Input (Data, Variable, Absolute)  ; modifier byte
    0x95, 0x01,                     //   Report Count (1)
    0x75, 0x08,                     //   Report Size (8)
    0x81, 0x01,                     //   Input (Constant)                  ; reserved byte
    0x95, 0x05,                     //   Report Count (5)
    0x75, 0x01,                     //   Report Size (1)
    0x05, 0x08,                     //   Usage Page (LEDs)
    0x19, 0x01,                     //   Usage Minimum (Num Lock)
    0x29, 0x05,                     //   Usage Maximum (Kana)
    0x91, 0x02,                     //   Output (Data, Variable, Absolute) ; LED bits
    0x95, 0x01,                     //   Report Count (1)
    0x75, 0x03,                     //   Report Size (3)
    0x91, 0x01,                     //   Output (Constant)                 ; LED padding
    0x95, 0x06,                     //   Report Count (6)
    0x75, 0x08,                     //   Report Size (8)
    0x15, 0x00,                     //   Logical Minimum (0)
    0x25, 0x65,                     //   Logical Maximum (101)
    0x05, 0x07,                     //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,                     //   Usage Minimum (0)
    0x29, 0x65,                     //   Usage Maximum (101)
    0x81, 0x00,                     //   Input (Data, Array)               ; 6 key codes
    0xC0,                           // End Collection

    /* Mouse: report ID 2, 5-byte input (buttons, X, Y, wheel, horizontal pan), 8-bit deltas */
    0x05, 0x01,                     // Usage Page (Generic Desktop)
    0x09, 0x02,                     // Usage (Mouse)
    0xA1, 0x01,                     // Collection (Application)
    0x85, HID_REPORT_ID_MOUSE,      //   Report ID (2)
    0x09, 0x01,                     //   Usage (Pointer)
    0xA1, 0x00,                     //   Collection (Physical)
    0x05, 0x09,                     //     Usage Page (Button)
    0x19, 0x01,                     //     Usage Minimum (Button 1)
    0x29, 0x05,                     //     Usage Maximum (Button 5)
    0x15, 0x00,                     //     Logical Minimum (0)
    0x25, 0x01,                     //     Logical Maximum (1)
    0x75, 0x01,                     //     Report Size (1)
    0x95, 0x05,                     //     Report Count (5)
    0x81, 0x02,                     //     Input (Data, Variable, Absolute)  ; 5 button bits
    0x75, 0x03,                     //     Report Size (3)
    0x95, 0x01,                     //     Report Count (1)
    0x81, 0x01,                     //     Input (Constant)                  ; padding
    0x05, 0x01,                     //     Usage Page (Generic Desktop)
    0x09, 0x30,                     //     Usage (X)
    0x09, 0x31,                     //     Usage (Y)
    0x09, 0x38,                     //     Usage (Wheel)
    0x15, 0x81,                     //     Logical Minimum (-127)
    0x25, 0x7F,                     //     Logical Maximum (127)
    0x75, 0x08,                     //     Report Size (8)
    0x95, 0x03,                     //     Report Count (3)
    0x81, 0x06,                     //     Input (Data, Variable, Relative)  ; X, Y, wheel
    0x05, 0x0C,                     //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,               //     Usage (AC Pan)
    0x15, 0x81,                     //     Logical Minimum (-127)
    0x25, 0x7F,                     //     Logical Maximum (127)
    0x75, 0x08,                     //     Report Size (8)
    0x95, 0x01,                     //     Report Count (1)
    0x81, 0x06,                     //     Input (Data, Variable, Relative)  ; horizontal pan
    0xC0,                           //   End Collection
    0xC0,                           // End Collection
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = s_report_map, .len = sizeof(s_report_map) },
};

static const esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x303A,            /* Espressif */
    .product_id = 0x4001,
    .version = 0x0100,
    .device_name = CONFIG_PHONEUSE_HID_DEVICE_NAME,
    .manufacturer_name = "Espressif",
    .serial_number = "PHONEUSE-0001",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

/* ---- advertising ---------------------------------------------------------------------- */

/* HID service UUID 0x1812 expressed as a 128-bit UUID (Bluedroid emits it as 16-bit). */
static uint8_t s_hid_service_uuid128[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = false,          /* name goes in the scan response to keep the ADV PDU small */
    .include_txpower = true,
    .min_interval = 0x0006,         /* preferred connection interval 7.5 ms .. 20 ms */
    .max_interval = 0x0010,
    .appearance = ESP_HID_APPEARANCE_MOUSE,
    .service_uuid_len = sizeof(s_hid_service_uuid128),
    .p_service_uuid = s_hid_service_uuid128,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static esp_ble_adv_data_t s_scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,            /* 20 ms .. 30 ms */
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* ---- state ---------------------------------------------------------------------------- */

static struct {
    SemaphoreHandle_t lock;
    esp_hidd_dev_t *dev;
    volatile bool connected;
    bool advertising;
    bool adv_data_ready;            /* advertising + scan response data configured */
    bool hid_started;               /* esp_hidd services registered */
} s_ble;

static void lock(void)
{
    xSemaphoreTake(s_ble.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_ble.lock);
}

/* Start advertising when the HID service is up, the ADV data is set and nobody is connected. */
static void maybe_start_advertising(void)
{
    bool start = false;
    lock();
    if (s_ble.adv_data_ready && s_ble.hid_started && !s_ble.connected && !s_ble.advertising) {
        s_ble.advertising = true;   /* provisional; cleared if the start fails */
        start = true;
    }
    unlock();
    if (start) {
        esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start advertising failed: %s", esp_err_to_name(err));
            lock();
            s_ble.advertising = false;
            unlock();
        }
    }
}

/* ---- GAP events ----------------------------------------------------------------------- */

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        if (param->adv_data_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "advertising data rejected: %d", param->adv_data_cmpl.status);
            break;
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ble_gap_config_adv_data(&s_scan_rsp_data));
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        if (param->scan_rsp_data_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "scan response data rejected: %d", param->scan_rsp_data_cmpl.status);
            break;
        }
        lock();
        s_ble.adv_data_ready = true;
        unlock();
        maybe_start_advertising();
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        lock();
        s_ble.advertising = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        unlock();
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "advertising as \"%s\"", CONFIG_PHONEUSE_HID_DEVICE_NAME);
        } else {
            ESP_LOGE(TAG, "advertising start failed: %d", param->adv_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        lock();
        s_ble.advertising = false;
        unlock();
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        /* The phone asks to secure the link: always accept. */
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        /* Numeric comparison (only with CONFIG_PHONEUSE_HID_BLE_MITM): confirm on our side. */
        ESP_LOGI(TAG, "pairing code %06" PRIu32 " - confirm on the phone", param->ble_security.key_notif.passkey);
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(TAG, "enter passkey %06" PRIu32 " on the phone", param->ble_security.key_notif.passkey);
        break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        ESP_LOGW(TAG, "phone asked for passkey entry, which this device cannot provide");
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "pairing complete (bonded peers: %d)", esp_ble_get_bond_device_num());
        } else {
            ESP_LOGE(TAG, "pairing failed, reason 0x%x", param->ble_security.auth_cmpl.fail_reason);
        }
        break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "connection params: interval %.2f ms, latency %u, timeout %u ms",
                 param->update_conn_params.conn_int * 1.25,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout * 10);
        break;

    case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
        ESP_LOGI(TAG, "bond removed (status %d)", param->remove_bond_dev_cmpl.status);
        break;

    default:
        ESP_LOGD(TAG, "GAP event %d", event);
        break;
    }
}

/* ---- esp_hidd events ------------------------------------------------------------------ */

static void hidd_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID service started");
        lock();
        s_ble.hid_started = true;
        unlock();
        maybe_start_advertising();
        break;

    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "host connected");
        lock();
        s_ble.connected = true;
        s_ble.advertising = false;      /* the controller stops advertising on connection */
        unlock();
        break;

    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "host disconnected: %s",
                 esp_hid_disconnect_reason_str(ESP_HID_TRANSPORT_BLE, param->disconnect.reason));
        lock();
        s_ble.connected = false;
        unlock();
        maybe_start_advertising();
        break;

    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG, "protocol mode: %s", param->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
        break;

    case ESP_HIDD_CONTROL_EVENT:
        ESP_LOGI(TAG, "host requested %s", param->control.control ? "exit suspend" : "suspend");
        break;

    case ESP_HIDD_OUTPUT_EVENT:
        /* Keyboard LED report (Caps Lock etc.). Nothing to do with it here. */
        ESP_LOGD(TAG, "output report id %u, %u bytes", param->output.report_id, param->output.length);
        break;

    case ESP_HIDD_STOP_EVENT:
        ESP_LOGW(TAG, "HID service stopped");
        lock();
        s_ble.hid_started = false;
        unlock();
        break;

    default:
        break;
    }
}

/* ---- security configuration ----------------------------------------------------------- */

static esp_err_t configure_security(void)
{
#if CONFIG_PHONEUSE_HID_BLE_MITM
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_IO;         /* numeric comparison, auto-confirmed here */
#else
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;       /* Just Works */
#endif
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req)), TAG, "auth req");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap)), TAG, "io cap");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)), TAG, "key size");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key)), TAG, "init key");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key)), TAG, "rsp key");
    return ESP_OK;
}

/* ---- transport interface -------------------------------------------------------------- */

esp_err_t hid_transport_init(void)
{
    ESP_RETURN_ON_FALSE(bt_stack_is_ready(), ESP_ERR_INVALID_STATE, TAG, "Bluetooth stack not started");
    ESP_RETURN_ON_FALSE(s_ble.dev == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    s_ble.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ble.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex");

    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_event_handler), TAG, "gap callback");
    ESP_RETURN_ON_ERROR(configure_security(), TAG, "security");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_device_name(CONFIG_PHONEUSE_HID_DEVICE_NAME), TAG, "device name");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler), TAG, "gatts callback");
    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_handler, &s_ble.dev),
                        TAG, "esp_hidd_dev_init");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_hidd_dev_battery_set(s_ble.dev, 100));
    ESP_RETURN_ON_ERROR(esp_ble_gap_config_adv_data(&s_adv_data), TAG, "adv data");
    return ESP_OK;
}

esp_err_t hid_transport_send_input(uint8_t report_id, const uint8_t *data, size_t len)
{
    if (!s_ble.dev || !s_ble.connected) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Blocks until Bluedroid confirms the notification was queued (ESP_GATTS_CONF_EVT). */
    esp_err_t err = esp_hidd_dev_input_set(s_ble.dev, 0, report_id, (uint8_t *)data, len);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

bool hid_transport_connected(void)
{
    return s_ble.connected;
}

void hid_transport_get_link(hid_transport_link_t *out)
{
    lock();
    out->connected = s_ble.connected;
    out->advertising = s_ble.advertising;
    unlock();
    int bonds = esp_ble_get_bond_device_num();
    out->bonded_peers = bonds < 0 ? 0 : bonds;
}

esp_err_t hid_transport_unpair_all(void)
{
    int count = esp_ble_get_bond_device_num();
    if (count <= 0) {
        return ESP_OK;
    }
    esp_ble_bond_dev_t *list = calloc((size_t)count, sizeof(*list));
    ESP_RETURN_ON_FALSE(list != NULL, ESP_ERR_NO_MEM, TAG, "bond list");
    esp_err_t err = esp_ble_get_bond_device_list(&count, list);
    if (err == ESP_OK) {
        for (int i = 0; i < count; i++) {
            ESP_LOGI(TAG, "removing bond " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(list[i].bd_addr));
            if (s_ble.connected) {
                /* Drop the live link so the phone notices it must pair again. */
                esp_ble_gap_disconnect(list[i].bd_addr);
            }
            esp_err_t e = esp_ble_remove_bond_device(list[i].bd_addr);
            if (e != ESP_OK && err == ESP_OK) {
                err = e;
            }
        }
    }
    free(list);
    return err;
}
