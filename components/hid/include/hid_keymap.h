/*
 * Key name / ASCII -> HID usage mapping (US keyboard layout). Pure C, no ESP-IDF dependencies,
 * so it is unit-tested on the host (see test/host).
 */
#pragma once

#include <stdbool.h>
#include "hid_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frequently used HID keyboard usage IDs. */
#define HID_KEY_ENTER      0x28
#define HID_KEY_ESCAPE     0x29
#define HID_KEY_BACKSPACE  0x2A
#define HID_KEY_TAB        0x2B
#define HID_KEY_SPACE      0x2C
#define HID_KEY_DELETE     0x4C
#define HID_KEY_RIGHT      0x4F
#define HID_KEY_LEFT       0x50
#define HID_KEY_DOWN       0x51
#define HID_KEY_UP         0x52

/**
 * @brief Map one ASCII character to a key (keycode + shift modifier).
 * @return false if the character has no key on a US layout (e.g. non-ASCII bytes).
 */
bool hid_keymap_from_ascii(char c, hid_key_t *out);

/**
 * @brief Parse a key specification such as "ENTER", "a", "A", "0x28", "CMD+H", "SHIFT+TAB".
 *        Names are case-insensitive; single characters are taken literally (A = Shift+a).
 *        Modifiers: CTRL, SHIFT, ALT/OPT/OPTION, CMD/COMMAND/GUI/META/WIN, and R-prefixed variants.
 * @return false if the specification is not understood.
 */
bool hid_keymap_from_name(const char *name, hid_key_t *out);

#ifdef __cplusplus
}
#endif
