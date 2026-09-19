#include <string.h>

#include "hid_keymap.h"
#include "input_records.h"

_Static_assert(sizeof(input_record_t) == INPUT_RECORD_SIZE, "input record layout");

#define ARDUINO_RAW_KEY_OFFSET 136      /* Arduino Keyboard: press(k) with k >= 136 sends usage k - 136 */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

input_hex_result_t input_records_decode(const char *hex, size_t hex_len, input_record_t *out, size_t *count)
{
    if (count) {
        *count = 0;
    }
    if (!hex || !out || hex_len == 0 || hex_len > INPUT_HEX_MAX || hex_len % (INPUT_RECORD_SIZE * 2) != 0) {
        return INPUT_HEX_BAD_LENGTH;
    }
    for (size_t i = 0; i < hex_len; i++) {
        if (hex_digit(hex[i]) < 0) {
            return INPUT_HEX_BAD_DIGIT;
        }
    }
    size_t n = hex_len / (INPUT_RECORD_SIZE * 2);
    uint8_t *bytes = (uint8_t *)out;
    for (size_t i = 0; i < n * INPUT_RECORD_SIZE; i++) {
        bytes[i] = (uint8_t)(hex_digit(hex[2 * i]) * 16 + hex_digit(hex[2 * i + 1]));
    }
    if (count) {
        *count = n;
    }
    return INPUT_HEX_OK;
}

static bool valid_key(uint8_t modifier, uint8_t key)
{
    return modifier <= INPUT_KEY_MODIFIER_MASK &&
           ((key >= 32 && key <= 126) || (key >= 176 && key <= 179) ||
            (key >= 194 && key <= 205) || (key >= 210 && key <= 218));
}

bool input_records_validate(const input_record_t *records, size_t count)
{
    if (!records || count == 0 || count > INPUT_RECORDS_MAX) {
        return false;
    }
    uint32_t waits = 0;
    uint8_t last_buttons = 0;
    for (size_t i = 0; i < count; i++) {
        const input_record_t *r = &records[i];
        switch (r->type) {
        case INPUT_RECORD_KEY:
            if (!valid_key(r->a, r->b) || r->c || r->d || last_buttons) {
                return false;
            }
            break;
        case INPUT_RECORD_MOUSE:
            if (r->a > 7 || r->b == 128 || r->c == 128 || r->d == 128) {
                return false;
            }
            last_buttons = r->a;
            break;
        case INPUT_RECORD_WAIT:
            if (r->c || r->d || last_buttons) {
                return false;
            }
            waits += input_record_wait_ms(r);
            if (waits > INPUT_WAIT_TOTAL_MAX_MS) {
                return false;
            }
            break;
        default:
            return false;
        }
    }
    /* Every batch must release its own buttons, including a drag. */
    return last_buttons == 0;
}

bool input_record_key(const input_record_t *record, hid_key_t *out)
{
    if (!record || !out || record->type != INPUT_RECORD_KEY || !valid_key(record->a, record->b)) {
        return false;
    }
    hid_key_t key = { 0, 0 };
    if (record->b >= ARDUINO_RAW_KEY_OFFSET) {
        key.keycode = (uint8_t)(record->b - ARDUINO_RAW_KEY_OFFSET);
    } else if (!hid_keymap_from_ascii((char)record->b, &key)) {
        return false;
    }
    key.modifiers |= (uint8_t)(record->a & INPUT_KEY_MODIFIER_MASK);
    *out = key;
    return true;
}

uint32_t input_record_wait_ms(const input_record_t *record)
{
    return record ? (uint32_t)record->a | ((uint32_t)record->b << 8) : 0;
}
