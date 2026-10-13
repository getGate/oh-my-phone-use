/*
 * Input-record protocol shared with the "iPad Computer Use" input tool (XIAO RP2040 firmware).
 * A batch is a string of hexadecimal five-byte records that is validated as a whole and then
 * played back in order:
 *
 *   Key    01 modifiers key 00 00    press + release; Arduino Keyboard key codes
 *   Mouse  02 buttons dx dy wheel    one mouse report; buttons persist (drag = held button,
 *                                    moves, then a record with buttons = 0)
 *   Wait   03 msLow msHigh 00 00     pause
 *
 * Limits (same as the reference firmware): at most 512 records, at most 10 s of waits, no
 * key or wait record while a button is held, and every batch releases its own buttons.
 *
 * Pure C, no ESP-IDF dependencies, unit-tested on the host (test/host).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid_device.h"     /* hid_key_t */

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_RECORD_SIZE        5
#define INPUT_RECORDS_MAX        512
#define INPUT_HEX_MAX            (INPUT_RECORDS_MAX * INPUT_RECORD_SIZE * 2)    /* 5120 */
#define INPUT_WAIT_TOTAL_MAX_MS  10000

enum {
    INPUT_RECORD_KEY = 1,
    INPUT_RECORD_MOUSE = 2,
    INPUT_RECORD_WAIT = 3,
};

/* Key-record modifier bits (Ctrl=1, Shift=2, Alt=4, Command=8): identical to HID_MOD_LEFT_*. */
#define INPUT_KEY_MODIFIER_MASK  0x0F

typedef struct {
    uint8_t type, a, b, c, d;
} input_record_t;

typedef enum {
    INPUT_HEX_OK = 0,
    INPUT_HEX_BAD_LENGTH,   /* empty, longer than INPUT_HEX_MAX, or not a multiple of 10 */
    INPUT_HEX_BAD_DIGIT,    /* a character that is not 0-9 / a-f / A-F */
} input_hex_result_t;

/**
 * @brief Decode a hexadecimal record string.
 * @param out    buffer for at least INPUT_RECORDS_MAX records
 * @param count  receives the number of decoded records (0 on error)
 */
input_hex_result_t input_records_decode(const char *hex, size_t hex_len, input_record_t *out, size_t *count);

/** Validate a decoded batch exactly like the reference firmware (validateInput). */
bool input_records_validate(const input_record_t *records, size_t count);

/**
 * @brief Translate a key record into a HID key (keycode + modifiers).
 *        Arduino key codes: 32..126 are ASCII (US layout, Shift added for capitals and symbols),
 *        codes >= 136 are raw HID usages + 136 (176 = Enter, 194 = F1, 218 = Up, ...).
 * @return false if the record is not a valid key record.
 */
bool input_record_key(const input_record_t *record, hid_key_t *out);

/** Duration of a wait record in milliseconds. */
uint32_t input_record_wait_ms(const input_record_t *record);

#ifdef __cplusplus
}
#endif
