#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "bt_stack.h"

static const char *TAG = "bt_stack";
static bool s_ready;

esp_err_t bt_stack_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if CONFIG_PHONEUSE_BT_KEEP_CLASSIC
    esp_bt_mode_t mode = ESP_BT_MODE_BTDM;
#else
    esp_bt_mode_t mode = ESP_BT_MODE_BLE;
    /* Give the Classic BT controller memory back to the heap; must happen before init. */
    ESP_RETURN_ON_ERROR(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT), TAG, "mem release");
#endif
#if CONFIG_IDF_TARGET_ESP32
    bt_cfg.mode = mode;
#endif
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), TAG, "controller init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(mode), TAG, "controller enable");

    esp_bluedroid_config_t host_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bluedroid_init_with_cfg(&host_cfg), TAG, "bluedroid init");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid enable");

    s_ready = true;
    ESP_LOGI(TAG, "Bluetooth ready (%s)", mode == ESP_BT_MODE_BTDM ? "BLE + Classic BT" : "BLE only");
    return ESP_OK;
}

bool bt_stack_is_ready(void)
{
    return s_ready;
}

bool bt_stack_classic_enabled(void)
{
#if CONFIG_PHONEUSE_BT_KEEP_CLASSIC
    return s_ready;
#else
    return false;
#endif
}
