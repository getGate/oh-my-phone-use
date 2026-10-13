#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "input_secret.h"

static const char *TAG = "secret";

#define NVS_NAMESPACE "phoneuse"
#define NVS_KEY       "secret"

static struct {
    SemaphoreHandle_t lock;
    char value[INPUT_SECRET_MAX_LEN + 1];
    bool from_nvs;
} s_secret;

static void lock(void)
{
    xSemaphoreTake(s_secret.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_secret.lock);
}

static void load_locked(void)
{
    s_secret.from_nvs = false;
    s_secret.value[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_secret.value);
        if (nvs_get_str(h, NVS_KEY, s_secret.value, &len) == ESP_OK && s_secret.value[0]) {
            s_secret.from_nvs = true;
        } else {
            s_secret.value[0] = '\0';
        }
        nvs_close(h);
    }
    if (!s_secret.from_nvs) {
        strncpy(s_secret.value, CONFIG_PHONEUSE_INPUT_TOOL_SECRET, sizeof(s_secret.value) - 1);
        s_secret.value[sizeof(s_secret.value) - 1] = '\0';
    }
}

esp_err_t input_secret_init(void)
{
    if (!s_secret.lock) {
        s_secret.lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_secret.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex");
    }
    lock();
    load_locked();
    bool configured = s_secret.value[0] != '\0';
    bool from_nvs = s_secret.from_nvs;
    unlock();
    if (configured) {
        ESP_LOGI(TAG, "input-device secret loaded from %s", from_nvs ? "NVS" : "build configuration");
    } else {
        ESP_LOGW(TAG, "no input-device secret configured: POST /input and /stop will be refused "
                      "until one is set (serial command: SECRET <value>)");
    }
    return ESP_OK;
}

bool input_secret_configured(void)
{
    if (!s_secret.lock) {
        return false;
    }
    lock();
    bool configured = s_secret.value[0] != '\0';
    unlock();
    return configured;
}

const char *input_secret_source(void)
{
    if (!s_secret.lock) {
        return "none";
    }
    lock();
    const char *source = !s_secret.value[0] ? "none" : (s_secret.from_nvs ? "nvs" : "build");
    unlock();
    return source;
}

bool input_secret_matches(const char *candidate)
{
    if (!candidate || !s_secret.lock) {
        return false;
    }
    lock();
    size_t expected_len = strlen(s_secret.value);
    size_t candidate_len = strlen(candidate);
    unsigned diff = (expected_len == 0 || expected_len != candidate_len) ? 1 : 0;
    for (size_t i = 0; i < expected_len && i < candidate_len; i++) {
        diff |= (unsigned)(s_secret.value[i] ^ candidate[i]);
    }
    unlock();
    return diff == 0;
}

esp_err_t input_secret_set(const char *secret)
{
    if (!s_secret.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (secret && strlen(secret) > INPUT_SECRET_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err;
    if (secret && *secret) {
        err = nvs_set_str(h, NVS_KEY, secret);
    } else {
        err = nvs_erase_key(h, NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs write");
    lock();
    load_locked();
    unlock();
    ESP_LOGI(TAG, "input-device secret %s", (secret && *secret) ? "stored in NVS" : "cleared from NVS");
    return ESP_OK;
}
