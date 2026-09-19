/*
 * Shared Bluetooth bring-up: controller + Bluedroid host.
 *
 * Kept separate from the HID transport so a future Classic Bluetooth profile (HFP) can be
 * started next to BLE HID without either module owning the stack.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise and enable the controller and the Bluedroid host. Idempotent. */
esp_err_t bt_stack_init(void);

/** True once bt_stack_init() has completed successfully. */
bool bt_stack_is_ready(void);

/** True if the controller was started in dual (BLE + Classic BT) mode. */
bool bt_stack_classic_enabled(void);

#ifdef __cplusplus
}
#endif
