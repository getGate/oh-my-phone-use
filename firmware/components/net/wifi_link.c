#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "wifi_link.h"

static const char *TAG = "wifi";

#define NVS_NAMESPACE   "phoneuse"
#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"
#define RETRY_MIN_MS    1000
#define RETRY_MAX_MS    ((uint32_t)CONFIG_PHONEUSE_WIFI_MAX_RETRY_DELAY_S * 1000)

static struct {
    SemaphoreHandle_t lock;
    esp_netif_t *netif;
    esp_timer_handle_t retry_timer;
    bool started;
    bool configured;
    bool connected;
    char ssid[33];
    char password[65];
    char ip[16];
    uint32_t retry_ms;
} s_wifi;

static void lock(void)
{
    xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_wifi.lock);
}

/* ---- credentials ---------------------------------------------------------------------- */

static void load_credentials(void)
{
    s_wifi.ssid[0] = s_wifi.password[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_wifi.ssid);
        if (nvs_get_str(h, NVS_KEY_SSID, s_wifi.ssid, &len) == ESP_OK && s_wifi.ssid[0]) {
            len = sizeof(s_wifi.password);
            if (nvs_get_str(h, NVS_KEY_PASS, s_wifi.password, &len) != ESP_OK) {
                s_wifi.password[0] = '\0';
            }
        } else {
            s_wifi.ssid[0] = '\0';
        }
        nvs_close(h);
    }
    if (!s_wifi.ssid[0]) {
        strncpy(s_wifi.ssid, CONFIG_PHONEUSE_WIFI_SSID, sizeof(s_wifi.ssid) - 1);
        strncpy(s_wifi.password, CONFIG_PHONEUSE_WIFI_PASSWORD, sizeof(s_wifi.password) - 1);
    }
    s_wifi.configured = s_wifi.ssid[0] != '\0';
}

static esp_err_t store_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err;
    if (ssid && *ssid) {
        err = nvs_set_str(h, NVS_KEY_SSID, ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, NVS_KEY_PASS, password ? password : "");
        }
    } else {
        err = nvs_erase_key(h, NVS_KEY_SSID);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            err = nvs_erase_key(h, NVS_KEY_PASS);
        }
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t apply_config(void)
{
    wifi_config_t cfg = { 0 };
    memcpy(cfg.sta.ssid, s_wifi.ssid, strnlen(s_wifi.ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, s_wifi.password, strnlen(s_wifi.password, sizeof(cfg.sta.password)));
    cfg.sta.threshold.authmode = s_wifi.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

/* ---- connection management ------------------------------------------------------------ */

static void connect_now(void)
{
    if (!s_wifi.started || !s_wifi.configured) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "connect: %s", esp_err_to_name(err));
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    connect_now();
}

static void schedule_retry(void)
{
    if (!s_wifi.configured) {
        return;
    }
    uint32_t delay = s_wifi.retry_ms;
    s_wifi.retry_ms = delay * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : delay * 2;
    esp_timer_stop(s_wifi.retry_timer);
    esp_timer_start_once(s_wifi.retry_timer, (uint64_t)delay * 1000);
    ESP_LOGI(TAG, "retrying in %lu ms", (unsigned long)delay);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_wifi.configured) {
                ESP_LOGI(TAG, "connecting to \"%s\"", s_wifi.ssid);
                connect_now();
            } else {
                ESP_LOGW(TAG, "no Wi-Fi credentials: send WIFI <ssid> <password> over the serial console");
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "associated with \"%s\"", s_wifi.ssid);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *e = data;
            lock();
            s_wifi.connected = false;
            s_wifi.ip[0] = '\0';
            unlock();
            ESP_LOGW(TAG, "disconnected from \"%s\" (reason %d)", s_wifi.ssid, e ? e->reason : -1);
            schedule_retry();
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            const ip_event_got_ip_t *e = data;
            lock();
            snprintf(s_wifi.ip, sizeof(s_wifi.ip), IPSTR, IP2STR(&e->ip_info.ip));
            s_wifi.connected = true;
            s_wifi.retry_ms = RETRY_MIN_MS;
            unlock();
            ESP_LOGI(TAG, "connected: http://%s/status (also http://%s.local/status)", s_wifi.ip,
                     CONFIG_PHONEUSE_WIFI_HOSTNAME);
        } else if (id == IP_EVENT_STA_LOST_IP) {
            lock();
            s_wifi.connected = false;
            s_wifi.ip[0] = '\0';
            unlock();
            ESP_LOGW(TAG, "IP address lost");
        }
    }
}

/* ---- public API ----------------------------------------------------------------------- */

esp_err_t wifi_link_start(void)
{
    ESP_RETURN_ON_FALSE(!s_wifi.started, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_wifi.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_wifi.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex");
    s_wifi.retry_ms = RETRY_MIN_MS;
    load_credentials();

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");
    s_wifi.netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_wifi.netif != NULL, ESP_FAIL, TAG, "sta netif");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_hostname(s_wifi.netif, CONFIG_PHONEUSE_WIFI_HOSTNAME));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG, "ip events");

    const esp_timer_create_args_t timer_args = { .callback = retry_timer_cb, .name = "wifi_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_wifi.retry_timer), TAG, "retry timer");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "sta mode");
    if (s_wifi.configured) {
        ESP_RETURN_ON_ERROR(apply_config(), TAG, "sta config");
    }
    s_wifi.started = true;
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
#if CONFIG_PHONEUSE_WIFI_POWER_SAVE
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
#else
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
#endif
    ESP_LOGI(TAG, "station started, hostname \"%s\"", CONFIG_PHONEUSE_WIFI_HOSTNAME);
    return ESP_OK;
}

esp_err_t wifi_link_set_credentials(const char *ssid, const char *password)
{
    if (!s_wifi.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    size_t pass_len = password ? strlen(password) : 0;
    if (ssid_len > 32 || pass_len > 63 || (ssid_len && pass_len && pass_len < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(store_credentials(ssid, password), TAG, "store credentials");

    lock();
    load_credentials();
    bool configured = s_wifi.configured;
    s_wifi.retry_ms = RETRY_MIN_MS;
    unlock();

    if (!s_wifi.started) {
        return ESP_OK;
    }
    esp_timer_stop(s_wifi.retry_timer);
    esp_wifi_disconnect();                          /* no-op when not connected */
    if (configured) {
        ESP_RETURN_ON_ERROR(apply_config(), TAG, "sta config");
        ESP_LOGI(TAG, "credentials updated, connecting to \"%s\"", s_wifi.ssid);
        connect_now();
    } else {
        ESP_LOGI(TAG, "credentials cleared");
    }
    return ESP_OK;
}

void wifi_link_get_status(wifi_link_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->hostname = CONFIG_PHONEUSE_WIFI_HOSTNAME;
    if (!s_wifi.lock) {
        return;
    }
    lock();
    out->configured = s_wifi.configured;
    out->connected = s_wifi.connected;
    memcpy(out->ssid, s_wifi.ssid, sizeof(out->ssid));
    memcpy(out->ip, s_wifi.ip, sizeof(out->ip));
    unlock();
    if (out->connected) {
        int rssi = 0;
        if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
            out->rssi = rssi;
        }
    }
}
