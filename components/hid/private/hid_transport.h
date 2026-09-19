/*
 * Internal contract between the logical HID layer (hid_device.c) and the Bluetooth HID
 * transport (hid_ble.c). Nothing outside components/hid includes this header.
 *
 * A future Classic Bluetooth HID transport, or a test double, only has to implement these
 * functions.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define HID_REPORT_ID_KEYBOARD   1
#define HID_REPORT_ID_MOUSE      2

#define HID_KEYBOARD_REPORT_LEN  8   /* modifiers, reserved, 6 key codes */
#define HID_MOUSE_REPORT_LEN     5   /* buttons, x, y, wheel, horizontal pan */

typedef struct {
    bool connected;
    bool advertising;
    int bonded_peers;
} hid_transport_link_t;

esp_err_t hid_transport_init(void);
esp_err_t hid_transport_send_input(uint8_t report_id, const uint8_t *data, size_t len);
bool hid_transport_connected(void);
void hid_transport_get_link(hid_transport_link_t *out);
esp_err_t hid_transport_unpair_all(void);
