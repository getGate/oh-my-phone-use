/*
 * Advertises the input tool with mDNS/Bonjour so the phone can reach it as
 * http://<CONFIG_PHONEUSE_WIFI_HOSTNAME>.local without knowing its IP address.
 * Services: _http._tcp and _phoneuse._tcp on the HTTP port.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mdns_advert_start(uint16_t http_port);

#ifdef __cplusplus
}
#endif
