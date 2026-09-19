#include "sdkconfig.h"
#include "boot_button.h"

#if CONFIG_PHONEUSE_INPUT_TOOL_BOOT_BUTTON

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hid_device.h"
#include "input_player.h"

static const char *TAG = "boot_btn";

#define POLL_MS      10
#define DEBOUNCE_US  30000

static void boot_button_task(void *arg)
{
    const gpio_num_t pin = (gpio_num_t)CONFIG_PHONEUSE_INPUT_TOOL_BOOT_BUTTON_GPIO;
    bool raw_prev = false, stable = false;
    int64_t changed_at = esp_timer_get_time();
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        bool raw = gpio_get_level(pin) == 0;            /* active low */
        int64_t now = esp_timer_get_time();
        if (raw != raw_prev) {
            raw_prev = raw;
            changed_at = now;
            continue;
        }
        if (now - changed_at < DEBOUNCE_US || raw == stable) {
            continue;
        }
        stable = raw;
        if (!stable) {
            continue;                                   /* released */
        }
        input_player_status_t player;
        hid_status_t hid;
        input_player_get_status(&player);
        hid_get_status(&hid);
        if (player.running || hid.mouse_buttons || !hid.connected) {
            ESP_LOGI(TAG, "BOOT pressed but the tool is busy or not connected; ignored");
            continue;
        }
        esp_err_t err = hid_click();
        ESP_LOGI(TAG, "BOOT pressed: left click %s", err == ESP_OK ? "sent" : esp_err_to_name(err));
    }
}

esp_err_t boot_button_start(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_PHONEUSE_INPUT_TOOL_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio config");
    BaseType_t ok = xTaskCreate(boot_button_task, "boot_button", 3072, NULL, 3, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "BOOT button (GPIO%d) sends a left click", CONFIG_PHONEUSE_INPUT_TOOL_BOOT_BUTTON_GPIO);
    return ESP_OK;
}

#else

esp_err_t boot_button_start(void)
{
    return ESP_OK;
}

#endif
