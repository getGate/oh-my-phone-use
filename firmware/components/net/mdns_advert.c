#include "sdkconfig.h"
#include "mdns_advert.h"

#if CONFIG_PHONEUSE_WIFI_MDNS

#include "esp_check.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "mdns";

esp_err_t mdns_advert_start(uint16_t http_port)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(CONFIG_PHONEUSE_WIFI_HOSTNAME), TAG, "hostname");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set("Oh My Phone Use ESP32 input tool"), TAG, "instance name");
    mdns_txt_item_t txt[] = {
        { "device", "Oh My Phone Use ESP32" },
        { "status", "/status" },
    };
    ESP_RETURN_ON_ERROR(mdns_service_add(NULL, "_http", "_tcp", http_port, txt, 2), TAG, "_http service");
    ESP_RETURN_ON_ERROR(mdns_service_add(NULL, "_phoneuse", "_tcp", http_port, txt, 2), TAG, "_phoneuse service");
    ESP_LOGI(TAG, "advertising %s.local", CONFIG_PHONEUSE_WIFI_HOSTNAME);
    return ESP_OK;
}

#else

esp_err_t mdns_advert_start(uint16_t http_port)
{
    (void)http_port;
    return ESP_OK;
}

#endif
