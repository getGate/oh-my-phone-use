/*
 * Logical HID API.
 *
 * This is the only interface the command layer (serial today, Wi-Fi later) uses.
 * Callers never see HID report formats, report IDs, delta limits or Bluetooth stack details:
 *   - large relative movements are split into multiple valid reports here,
 *   - text is translated into key press/release sequences here,
 *   - the Bluetooth transport lives behind hid_transport.h (see hid_ble.c).
 *
 * All functions are thread-safe (internally serialised by a mutex) and must be called from a
 * task context, never from a Bluetooth callback: sending a report blocks briefly until the
 * stack has queued it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mouse button bits (HID Button usage page, buttons 1..3). */
#define HID_MOUSE_BUTTON_LEFT    0x01
#define HID_MOUSE_BUTTON_RIGHT   0x02
#define HID_MOUSE_BUTTON_MIDDLE  0x04

/* Keyboard modifier bits (USB HID usage page 0x07, usages 0xE0..0xE7). */
#define HID_MOD_LEFT_CTRL    0x01
#define HID_MOD_LEFT_SHIFT   0x02
#define HID_MOD_LEFT_ALT     0x04
#define HID_MOD_LEFT_GUI     0x08  /* Command key on Apple platforms */
#define HID_MOD_RIGHT_CTRL   0x10
#define HID_MOD_RIGHT_SHIFT  0x20
#define HID_MOD_RIGHT_ALT    0x40
#define HID_MOD_RIGHT_GUI    0x80

/* Largest absolute value accepted by hid_move() / hid_scroll(). */
#define HID_MAX_DELTA 32767

/** A key as understood by the logical API: usage ID plus modifier mask. */
typedef struct {
    uint8_t keycode;    /*!< HID keyboard usage ID, 0 = none (modifier-only key) */
    uint8_t modifiers;  /*!< HID_MOD_* bitmask */
} hid_key_t;

typedef enum {
    HID_LINK_DOWN = 0,      /*!< Bluetooth HID service not running */
    HID_LINK_ADVERTISING,   /*!< waiting for the phone to connect */
    HID_LINK_CONNECTED,     /*!< phone connected; reports are delivered once it has paired/subscribed */
} hid_link_state_t;

/** Snapshot of the HID state, safe to read from any task. */
typedef struct {
    hid_link_state_t link;
    bool connected;         /*!< a host is connected over Bluetooth */
    bool advertising;       /*!< currently advertising for a connection */
    int bonded_peers;       /*!< number of bonded (paired) hosts stored in NVS */
    uint8_t mouse_buttons;  /*!< HID_MOUSE_BUTTON_* bits currently held */
    uint32_t reports_sent;  /*!< input reports accepted by the stack since boot */
    uint32_t reports_failed;/*!< input reports the stack refused since boot */
} hid_status_t;

/**
 * @brief Start the HID device (Bluetooth transport, GATT services, advertising).
 *        bt_stack_init() must have completed first.
 */
esp_err_t hid_device_init(void);

void hid_get_status(hid_status_t *out);
bool hid_is_connected(void);

/* ---- Mouse ---------------------------------------------------------------------------- */

/** Relative move. Any |dx|,|dy| <= HID_MAX_DELTA; split into as many reports as needed. */
esp_err_t hid_move(int dx, int dy);
/** Left button press followed by release (a tap under AssistiveTouch). */
esp_err_t hid_click(void);
esp_err_t hid_mouse_down(void);     /*!< hold the left button (drag/swipe with hid_move) */
esp_err_t hid_mouse_up(void);
/** Vertical scroll; positive = wheel up. Split into as many reports as needed. */
esp_err_t hid_scroll(int dy);
/**
 * @brief One raw mouse report: explicit button state plus 8-bit deltas, exactly like a USB
 *        mouse report. The button state persists for later reports (a drag is a held button,
 *        moves, then a report with buttons = 0). Used by the input-tool record player.
 */
esp_err_t hid_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);

/* ---- Keyboard ------------------------------------------------------------------------- */

esp_err_t hid_key_press(hid_key_t key);   /*!< press + release */
esp_err_t hid_key_down(hid_key_t key);    /*!< hold (up to 6 keys plus modifiers) */
esp_err_t hid_key_up(hid_key_t key);
esp_err_t hid_release_all(void);          /*!< release every key and mouse button */
/**
 * @brief Type ASCII text (US keyboard layout). '\n' = Enter, '\t' = Tab, '\b' = Backspace.
 * @return ESP_ERR_NOT_SUPPORTED (nothing typed) if the text contains an untypeable character.
 */
esp_err_t hid_type_text(const char *text);

/* ---- Pairing -------------------------------------------------------------------------- */

/** Remove all bonded hosts so the phone has to pair again (forget the device on the phone too). */
esp_err_t hid_unpair_all(void);

/*
 * Error codes used by the functions above:
 *   ESP_ERR_INVALID_STATE  no host connected (or HID not initialised)
 *   ESP_ERR_INVALID_ARG    argument out of range
 *   ESP_ERR_NOT_SUPPORTED  text contains a character with no US-layout key
 *   ESP_ERR_INVALID_SIZE   more than 6 keys held at once
 *   ESP_FAIL               the Bluetooth stack refused the report
 */

#ifdef __cplusplus
}
#endif
