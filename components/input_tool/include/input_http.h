/*
 * HTTP front end implementing the iPad Computer Use input-device protocol
 * (input_device/PROTOCOL.md of that project):
 *
 *   GET  /        device description
 *   GET  /status  {"device","transport","running","state","waitMs","hidReady","reports",...}
 *   POST /input   hexadecimal records -> 202 queued | 400 invalid | 403 secret | 409 busy
 *   POST /stop    release everything and stop -> 200
 *
 * Mutating requests must carry the X-Input-Device-Secret header (see input_secret.h).
 * The server listens on every interface from boot; it becomes reachable once Wi-Fi is up.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t input_http_start(void);

#ifdef __cplusplus
}
#endif
