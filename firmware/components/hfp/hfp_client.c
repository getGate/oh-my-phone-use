#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "bt_stack.h"
#include "call_audio.h"
#include "hfp_client.h"

static const char *TAG = "hfp";

#define UPLINK_TASK_STACK    3584
#define UPLINK_TASK_PRIO     6
#define UPLINK_POLL_MS       10
#define RECONNECT_PERIOD_US  (15 * 1000 * 1000)
#define COD_MINOR_HANDS_FREE 0x02
#define NUMBER_MAX           32

typedef struct {
    esp_hf_client_connection_state_t conn;
    esp_hf_client_audio_state_t audio;
    bool call_active;
    int call_setup;                         /* esp_hf_call_setup_status_t */
    int call_held;
    char number[NUMBER_MAX + 1];            /* last caller ID */
    esp_bd_addr_t peer;
    bool peer_valid;
    uint16_t frame_size;                    /* preferred audio frame size, bytes */
} hfp_state_t;

static hfp_state_t s_state;
static SemaphoreHandle_t s_lock;
static hfp_line_sink_t s_sink;
static esp_timer_handle_t s_reconnect_timer;
static atomic_bool s_cvsd;                  /* the audio link is up and carries PCM */
static atomic_uint_least32_t s_tx_send_failed;

static const char *const CONN_NAMES[] = { "disconnected", "connecting", "connected", "slc_connected", "disconnecting" };
static const char *const AUDIO_NAMES[] = { "disconnected", "connecting", "connected", "connected_msbc" };
static const char *const SETUP_NAMES[] = { "none", "incoming", "dialing", "alerting" };

#define NAME_OF(table, index) \
    (((unsigned)(index) < sizeof(table) / sizeof((table)[0])) ? (table)[(unsigned)(index)] : "unknown")

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void emit(const char *fmt, ...)
{
    if (!s_sink) {
        return;
    }
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    s_sink(line);
}

static char *bda_str(const uint8_t *bda, char *out, size_t size)
{
    snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return out;
}

static void get_state(hfp_state_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

static bool slc_up(void)
{
    hfp_state_t st;
    get_state(&st);
    return st.conn == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED;
}

/* ---- audio ----------------------------------------------------------------------------- */

/*
 * Runs in the Bluetooth host for every received audio packet. The packet of the phone is
 * consumed first, then the same buffer carries the next packet to the phone, so the send
 * rate follows the clock of the audio link.
 */
static void audio_data_cb(esp_hf_sync_conn_hdl_t hdl, esp_hf_audio_buff_t *buf, bool is_bad_frame)
{
    uint16_t len = buf->data_len & (uint16_t)~1U;
    if (!atomic_load(&s_cvsd) || len == 0) {
        esp_hf_client_audio_buff_free(buf);
        return;
    }
    call_audio_rx(buf->data, len, is_bad_frame);
    call_audio_tx(buf->data, len);
    buf->data_len = len;
    if (esp_hf_client_audio_data_send(hdl, buf) != ESP_OK) {
        esp_hf_client_audio_buff_free(buf);
        atomic_fetch_add(&s_tx_send_failed, 1);
    }
}

static void uplink_task(void *arg)
{
    static char line[CALL_AUDIO_LINE_MAX];
    for (;;) {
        while (call_audio_host_out(line, sizeof(line)) > 0) {
            if (s_sink) {
                s_sink(line);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(UPLINK_POLL_MS));
    }
}

/* ---- connection ------------------------------------------------------------------------ */

static void set_discoverable(bool on)
{
    /* While a phone is connected, page and inquiry scans only take radio time from the audio. */
    esp_bt_gap_set_scan_mode(on ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
                             on ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
}

/* A car kit connects to its phone after power-up; the iPhone does not always do it by itself. */
static void reconnect_timer_cb(void *arg)
{
    hfp_state_t st;
    get_state(&st);
    if (st.conn != ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
        return;
    }
    esp_bd_addr_t bonded[1];
    int count = 1;
    if (esp_bt_gap_get_bond_device_num() < 1 ||
        esp_bt_gap_get_bond_device_list(&count, bonded) != ESP_OK || count < 1) {
        return;
    }
    char addr[18];
    ESP_LOGI(TAG, "connecting to bonded phone %s", bda_str(bonded[0], addr, sizeof(addr)));
    esp_hf_client_connect(bonded[0]);
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    char addr[18];
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        bda_str(param->auth_cmpl.bda, addr, sizeof(addr));
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "pairing complete with %s (%s)", addr, (const char *)param->auth_cmpl.device_name);
            emit("EVT PAIR result=ok peer=%s", addr);
        } else {
            ESP_LOGW(TAG, "pairing failed with %s, status %d", addr, param->auth_cmpl.stat);
            emit("EVT PAIR result=fail peer=%s status=%d", addr, param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "pairing code %06" PRIu32 ", confirmed automatically", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "pairing passkey %06" PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        /* Legacy pairing (not used by iOS): fixed PIN 0000, or sixteen zeros when asked for. */
        esp_bt_pin_code_t pin = { '0', '0', '0', '0' };
        if (param->pin_req.min_16_digit) {
            memset(pin, '0', sizeof(pin));
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin);
        } else {
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        }
        break;
    }
    default:
        break;
    }
}

static void on_connection_state(const esp_hf_client_cb_param_t *param)
{
    esp_hf_client_connection_state_t state = param->conn_stat.state;
    char addr[18];
    bda_str(param->conn_stat.remote_bda, addr, sizeof(addr));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.conn = state;
    memcpy(s_state.peer, param->conn_stat.remote_bda, sizeof(s_state.peer));
    s_state.peer_valid = true;
    if (state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
        s_state.call_active = false;
        s_state.call_setup = ESP_HF_CALL_SETUP_STATUS_IDLE;
        s_state.call_held = 0;
        s_state.number[0] = '\0';
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "phone %s: %s (peer features 0x%" PRIx32 ")", addr, NAME_OF(CONN_NAMES, state),
             param->conn_stat.peer_feat);
    emit("EVT SLC state=%s peer=%s", NAME_OF(CONN_NAMES, state), addr);

    if (state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
        set_discoverable(false);
        /* The audio comes from a machine: ask the phone not to run its echo / noise processing. */
        esp_hf_client_send_nrec();
    } else if (state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
        set_discoverable(true);
    }
}

static void on_audio_state(const esp_hf_client_cb_param_t *param)
{
    esp_hf_client_audio_state_t state = param->audio_stat.state;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.audio = state;
    s_state.frame_size = param->audio_stat.preferred_frame_size;
    xSemaphoreGive(s_lock);

    if (state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
        call_audio_link_open();
        atomic_store(&s_tx_send_failed, 0);
        atomic_store(&s_cvsd, true);
        esp_hf_client_register_audio_data_callback(audio_data_cb);
    } else {
        atomic_store(&s_cvsd, false);
        if (state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            call_audio_link_close();
        } else if (state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            ESP_LOGW(TAG, "the phone selected mSBC; this build only handles CVSD (disable CONFIG_BT_HFP_WBS_ENABLE)");
        }
    }
    ESP_LOGI(TAG, "audio link: %s, frame %u bytes", NAME_OF(AUDIO_NAMES, state),
             (unsigned)param->audio_stat.preferred_frame_size);
    emit("EVT AUDIO state=%s frame=%u", NAME_OF(AUDIO_NAMES, state),
         (unsigned)param->audio_stat.preferred_frame_size);
}

static void hf_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    switch (event) {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        on_connection_state(param);
        break;
    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        on_audio_state(param);
        break;
    case ESP_HF_CLIENT_CIND_CALL_EVT:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.call_active = param->call.status == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "call %s", param->call.status ? "active" : "ended");
        emit("EVT CALL active=%d", param->call.status ? 1 : 0);
        break;
    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.call_setup = param->call_setup.status;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "call setup: %s", NAME_OF(SETUP_NAMES, param->call_setup.status));
        emit("EVT SETUP state=%s", NAME_OF(SETUP_NAMES, param->call_setup.status));
        break;
    case ESP_HF_CLIENT_CIND_CALL_HELD_EVT:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.call_held = param->call_held.status;
        xSemaphoreGive(s_lock);
        emit("EVT HELD state=%d", (int)param->call_held.status);
        break;
    case ESP_HF_CLIENT_RING_IND_EVT:
        emit("EVT RING");
        break;
    case ESP_HF_CLIENT_CLIP_EVT:
        if (param->clip.number) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strlcpy(s_state.number, param->clip.number, sizeof(s_state.number));
            xSemaphoreGive(s_lock);
            emit("EVT CLIP number=%s", param->clip.number);
        }
        break;
    case ESP_HF_CLIENT_BSIR_EVT:
        ESP_LOGI(TAG, "in-band ring tone: %s", param->bsir.state ? "provided" : "not provided");
        break;
    case ESP_HF_CLIENT_AT_RESPONSE_EVT:
        if (param->at_response.code != ESP_HF_AT_RESPONSE_CODE_OK) {
            ESP_LOGW(TAG, "AT command refused by the phone: code %d, cme %d",
                     param->at_response.code, param->at_response.cme);
            emit("EVT AT_ERROR code=%d cme=%d", param->at_response.code, param->at_response.cme);
        }
        break;
    case ESP_HF_CLIENT_PROF_STATE_EVT:
        ESP_LOGI(TAG, "profile state %d", param->prof_stat.state);
        break;
    default:
        break;
    }
}

/* ---- API ------------------------------------------------------------------------------- */

static size_t clamp_len(int n, size_t size)
{
    if (n < 0 || size == 0) {
        return 0;
    }
    return (size_t)n < size ? (size_t)n : size - 1;
}

size_t hfp_client_call_status(char *out, size_t size)
{
    hfp_state_t st;
    get_state(&st);
    char addr[18] = "-";
    if (st.peer_valid) {
        bda_str(st.peer, addr, sizeof(addr));
    }
    const char *audio = st.audio == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ? "cvsd" :
                        st.audio == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC ? "msbc" : "off";
    return clamp_len(snprintf(out, size, "slc=%d audio=%s call=%d setup=%s held=%d number=%s peer=%s bonds=%d",
                              st.conn == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED ? 1 : 0, audio,
                              st.call_active ? 1 : 0, NAME_OF(SETUP_NAMES, st.call_setup), st.call_held,
                              st.number[0] ? st.number : "-", addr, esp_bt_gap_get_bond_device_num()), size);
}

size_t hfp_client_audio_status(char *out, size_t size)
{
    call_audio_stats_t a;
    call_audio_get_stats(&a, true);
    return clamp_len(snprintf(out, size,
                              "mode=%s link=%d rate=%d rx=%" PRIu32 " rx_bad=%" PRIu32 " rx_drop=%" PRIu32
                              " tx=%" PRIu32 " tx_underrun=%" PRIu32 " tx_drop=%" PRIu32 " tx_fail=%" PRIu32
                              " host_in=%" PRIu32 " host_bad=%" PRIu32 " host_out=%" PRIu32 " peak=%u",
                              call_audio_mode_name(a.mode), a.link_open ? 1 : 0, CALL_AUDIO_SAMPLE_RATE,
                              a.rx_frames, a.rx_bad_frames, a.rx_dropped, a.tx_frames, a.tx_underruns,
                              a.tx_dropped, (uint32_t)atomic_load(&s_tx_send_failed), a.host_lines_in,
                              a.host_lines_bad, a.host_lines_out, (unsigned)a.rx_peak), size);
}

esp_err_t hfp_client_dial(const char *number)
{
    if (!number || !number[0] || strlen(number) > NUMBER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!slc_up()) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_hf_client_dial(number);
}

esp_err_t hfp_client_answer(void)
{
    return slc_up() ? esp_hf_client_answer_call() : ESP_ERR_INVALID_STATE;
}

esp_err_t hfp_client_hangup(void)
{
    return slc_up() ? esp_hf_client_reject_call() : ESP_ERR_INVALID_STATE;
}

esp_err_t hfp_client_audio_link(bool connect)
{
    hfp_state_t st;
    get_state(&st);
    if (st.conn != ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    return connect ? esp_hf_client_connect_audio(st.peer) : esp_hf_client_disconnect_audio(st.peer);
}

esp_err_t hfp_client_unpair_all(void)
{
    int count = esp_bt_gap_get_bond_device_num();
    if (count <= 0) {
        return ESP_OK;
    }
    esp_bd_addr_t *list = calloc((size_t)count, sizeof(esp_bd_addr_t));
    if (!list) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_bt_gap_get_bond_device_list(&count, list);
    for (int i = 0; err == ESP_OK && i < count; i++) {
        err = esp_bt_gap_remove_bond_device(list[i]);
    }
    free(list);
    return err;
}

esp_err_t hfp_client_start(hfp_line_sink_t sink)
{
    ESP_RETURN_ON_FALSE(bt_stack_classic_enabled(), ESP_ERR_INVALID_STATE, TAG, "Classic Bluetooth is not running");
    ESP_RETURN_ON_FALSE(s_lock == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex");
    s_sink = sink;
    call_audio_init();

    ESP_RETURN_ON_ERROR(esp_bt_gap_set_device_name(CONFIG_PHONEUSE_HFP_DEVICE_NAME), TAG, "device name");
    ESP_RETURN_ON_ERROR(esp_bt_gap_register_callback(gap_cb), TAG, "gap callback");
    ESP_RETURN_ON_ERROR(esp_hf_client_register_callback(hf_cb), TAG, "hf callback");
    ESP_RETURN_ON_ERROR(esp_hf_client_init(), TAG, "hf client init");
    ESP_RETURN_ON_ERROR(esp_hf_client_register_audio_data_callback(audio_data_cb), TAG, "audio callback");

    /* "Just Works" secure simple pairing, like the BLE HID side: the iPhone only asks to pair. */
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t)), TAG, "io capability");
    esp_bt_pin_code_t pin = { '0', '0', '0', '0' };
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin), TAG, "pin");

    /* Present the board as a hands-free audio device, so that the iPhone lists it and routes calls to it. */
    esp_bt_cod_t cod = {
        .major = ESP_BT_COD_MAJOR_DEV_AV,
        .minor = COD_MINOR_HANDS_FREE,
        .service = ESP_BT_COD_SRVC_AUDIO,
    };
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD), TAG, "class of device");
    set_discoverable(true);

    BaseType_t ok = xTaskCreate(uplink_task, "hfp_uplink", UPLINK_TASK_STACK, NULL, UPLINK_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "uplink task");

    const esp_timer_create_args_t timer_args = { .callback = reconnect_timer_cb, .name = "hfp_reconnect" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer), TAG, "reconnect timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_reconnect_timer, RECONNECT_PERIOD_US), TAG, "reconnect timer start");

    ESP_LOGI(TAG, "hands-free unit \"%s\" ready, %d bonded phone(s)", CONFIG_PHONEUSE_HFP_DEVICE_NAME,
             esp_bt_gap_get_bond_device_num());
    return ESP_OK;
}
