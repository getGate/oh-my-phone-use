/*
 * HID transport for builds without Bluetooth HID (CONFIG_PHONEUSE_HID_ENABLE=n, for example
 * the HFP call audio test). No host is ever connected, so the HID commands answer
 * "ERR not_connected" and the rest of the firmware stays unchanged.
 */
#include <string.h>

#include "esp_log.h"

#include "hid_transport.h"

static const char *TAG = "hid_null";

esp_err_t hid_transport_init(void)
{
    ESP_LOGW(TAG, "Bluetooth HID is disabled in this build");
    return ESP_OK;
}

esp_err_t hid_transport_send_input(uint8_t report_id, const uint8_t *data, size_t len)
{
    (void)report_id;
    (void)data;
    (void)len;
    return ESP_ERR_INVALID_STATE;
}

bool hid_transport_connected(void)
{
    return false;
}

void hid_transport_get_link(hid_transport_link_t *out)
{
    memset(out, 0, sizeof(*out));
}

esp_err_t hid_transport_unpair_all(void)
{
    return ESP_OK;
}
