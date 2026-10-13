/*
 * Wi-Fi station link for the input-tool HTTP API.
 *
 * Credentials come from NVS (WIFI <ssid> <password> serial command, survives reflashing) or,
 * when nothing is stored, from CONFIG_PHONEUSE_WIFI_SSID / CONFIG_PHONEUSE_WIFI_PASSWORD.
 * The station reconnects on its own with exponential backoff. The hostname
 * (CONFIG_PHONEUSE_WIFI_HOSTNAME) is used for DHCP and, with mdns_advert.h, for
 * <hostname>.local resolution on the phone.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool configured;        /*!< credentials are available (NVS or build configuration) */
    bool connected;         /*!< associated and holding an IPv4 address */
    char ssid[33];
    char ip[16];
    int rssi;               /*!< dBm, valid while connected */
    const char *hostname;
} wifi_link_status_t;

esp_err_t wifi_link_start(void);

/**
 * @brief Store credentials in NVS and (re)connect. ssid NULL or "" forgets the stored
 *        credentials and disconnects. password "" means an open network.
 */
esp_err_t wifi_link_set_credentials(const char *ssid, const char *password);

void wifi_link_get_status(wifi_link_status_t *out);

#ifdef __cplusplus
}
#endif
