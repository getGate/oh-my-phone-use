/*
 * The BOOT button (GPIO0) sends a left click while nothing else is running. iOS asks you to
 * press a button on the pointing device when assigning AssistiveTouch actions
 * (Settings > Accessibility > Touch > AssistiveTouch > Devices > Customize Additional Buttons).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t boot_button_start(void);

#ifdef __cplusplus
}
#endif
