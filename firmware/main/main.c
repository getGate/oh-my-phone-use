/*
 * PhoneUse firmware entry point: an ESP32-PICO-KIT-1 as a Bluetooth LE mouse + keyboard for an
 * iPhone/iPad, driven either by serial commands or by the "iPad Computer Use" input-tool
 * protocol over Wi-Fi.
 *
 *   Mac (USB serial) -> serial_console -> command_parser --+
 *                                                          +-> hid_device (logical HID API)
 *   iPad app (Wi-Fi) -> input_http -> input_player --------+        |
 *                                                               hid_ble (BLE HID transport)
 *                                                                    |
 *                                                               bt_stack (controller + Bluedroid)
 *
 * app_main only wires the modules together.
 */
#include <stdio.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#if CONFIG_PHONEUSE_HEAP_DIAGNOSTICS
#include "esp_heap_caps.h"
#endif

#include "bt_stack.h"
#include "command_parser.h"
#include "hid_device.h"
#include "serial_console.h"

#if CONFIG_PHONEUSE_INPUT_TOOL_ENABLE
#include "boot_button.h"
#include "input_http.h"
#include "input_player.h"
#include "input_secret.h"
#include "mdns_advert.h"
#include "wifi_link.h"
#endif

#if CONFIG_PHONEUSE_HFP_ENABLE
#include "hfp_client.h"
#endif

static const char *TAG = "main";

#if CONFIG_PHONEUSE_HEAP_DIAGNOSTICS
static void log_heap(void *arg)
{
    (void)arg;
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG, "HEAP internal_free=%u internal_min=%u internal_largest=%u",
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps));
}
#endif

static uint32_t uptime_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

#if CONFIG_PHONEUSE_INPUT_TOOL_ENABLE

static size_t clamp_len(int n, size_t size)
{
    if (n < 0) {
        return 0;
    }
    return (size_t)n < size ? (size_t)n : size - 1;
}

static size_t net_status_fields(char *out, size_t size)
{
    wifi_link_status_t wifi;
    input_player_status_t player;
    wifi_link_get_status(&wifi);
    input_player_get_status(&player);
    return clamp_len(snprintf(out, size, " wifi=%d ip=%s player=%s",
                              wifi.connected ? 1 : 0, wifi.connected ? wifi.ip : "-",
                              input_player_state_name(player.state)), size);
}

static size_t wifi_status_body(char *out, size_t size)
{
    wifi_link_status_t wifi;
    wifi_link_get_status(&wifi);
    return clamp_len(snprintf(out, size, "configured=%d connected=%d ssid=%s ip=%s rssi=%d hostname=%s.local",
                              wifi.configured ? 1 : 0, wifi.connected ? 1 : 0,
                              wifi.configured ? wifi.ssid : "-", wifi.connected ? wifi.ip : "-",
                              wifi.connected ? wifi.rssi : 0, wifi.hostname), size);
}

static size_t secret_status_body(char *out, size_t size)
{
    return clamp_len(snprintf(out, size, "configured=%d source=%s",
                              input_secret_configured() ? 1 : 0, input_secret_source()), size);
}

static void start_input_tool(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(input_secret_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(input_player_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_link_start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_advert_start(CONFIG_PHONEUSE_INPUT_TOOL_HTTP_PORT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(input_http_start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(boot_button_start());
}

#endif /* CONFIG_PHONEUSE_INPUT_TOOL_ENABLE */

#if CONFIG_PHONEUSE_HFP_ENABLE

static esp_err_t call_control(command_call_op_t op, const char *number)
{
    switch (op) {
    case COMMAND_CALL_DIAL:   return hfp_client_dial(number);
    case COMMAND_CALL_ANSWER: return hfp_client_answer();
    case COMMAND_CALL_HANGUP: return hfp_client_hangup();
    default:                  return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t audio_control(command_audio_op_t op)
{
    switch (op) {
    case COMMAND_AUDIO_OFF:        call_audio_set_mode(CALL_AUDIO_OFF); return ESP_OK;
    case COMMAND_AUDIO_TONE:       call_audio_set_mode(CALL_AUDIO_TONE); return ESP_OK;
    case COMMAND_AUDIO_LOOP:       call_audio_set_mode(CALL_AUDIO_LOOP); return ESP_OK;
    case COMMAND_AUDIO_STREAM:     call_audio_set_mode(CALL_AUDIO_STREAM); return ESP_OK;
    case COMMAND_AUDIO_CONNECT:    return hfp_client_audio_link(true);
    case COMMAND_AUDIO_DISCONNECT: return hfp_client_audio_link(false);
    default:                       return ESP_ERR_INVALID_ARG;
    }
}

static void audio_in(const char *payload)
{
    call_audio_host_in(payload);
}

#endif /* CONFIG_PHONEUSE_HFP_ENABLE */

void app_main(void)
{
    /* NVS holds the BLE bonding keys, the Wi-Fi credentials and the input-device secret. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Serial console first: PING/STATUS work even while Bluetooth is still starting. */
    static const command_platform_t platform = {
        .uptime_s = uptime_seconds,
#if CONFIG_PHONEUSE_INPUT_TOOL_ENABLE
        .net_status = net_status_fields,
        .wifi_status = wifi_status_body,
        .wifi_set = wifi_link_set_credentials,
        .secret_status = secret_status_body,
        .secret_set = input_secret_set,
#endif
#if CONFIG_PHONEUSE_HFP_ENABLE
        .call_status = hfp_client_call_status,
        .call_control = call_control,
        .audio_status = hfp_client_audio_status,
        .audio_control = audio_control,
        .audio_in = audio_in,
        .call_unpair = hfp_client_unpair_all,
#endif
    };
    command_parser_init(&platform);
    ESP_ERROR_CHECK(serial_console_start());

    ESP_ERROR_CHECK(bt_stack_init());
    ESP_ERROR_CHECK(hid_device_init());
#if CONFIG_PHONEUSE_HFP_ENABLE
    /* Events ("EVT ...") and call audio ("A ...") go to the host as unsolicited console lines. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(hfp_client_start(serial_console_write_line));
#endif

#if CONFIG_PHONEUSE_INPUT_TOOL_ENABLE
    start_input_tool();
    ESP_LOGI(TAG, "Oh My Phone Use ready: BLE HID \"%s\", input-tool API on port %d",
             CONFIG_PHONEUSE_HID_DEVICE_NAME, CONFIG_PHONEUSE_INPUT_TOOL_HTTP_PORT);
#elif CONFIG_PHONEUSE_HID_ENABLE
    ESP_LOGI(TAG, "Oh My Phone Use HID ready (device name \"%s\"), Wi-Fi input tool disabled",
             CONFIG_PHONEUSE_HID_DEVICE_NAME);
#else
    ESP_LOGI(TAG, "Bluetooth HID and the Wi-Fi input tool are disabled in this build");
#endif
#if CONFIG_PHONEUSE_HFP_ENABLE
    ESP_LOGI(TAG, "Oh My Phone Use call audio ready: hands-free unit \"%s\"", CONFIG_PHONEUSE_HFP_DEVICE_NAME);
#endif
    ESP_LOGI(TAG, "free heap after start-up: %lu bytes", (unsigned long)esp_get_free_heap_size());
#if CONFIG_PHONEUSE_HEAP_DIAGNOSTICS
    static esp_timer_handle_t heap_timer;
    const esp_timer_create_args_t args = { .callback = log_heap, .name = "heap_stats" };
    ESP_ERROR_CHECK(esp_timer_create(&args, &heap_timer));
    log_heap(NULL);
    ESP_ERROR_CHECK(esp_timer_start_periodic(heap_timer, 5000000));
#endif
}
