/* Records every logical HID call so the parser tests can assert on them. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hid_device.h"
#include "hid_keymap.h"
#include "mock_hid.h"

char mock_calls[4096];
bool mock_connected = true;
esp_err_t mock_force_error = ESP_OK;

void mock_reset(void)
{
    mock_calls[0] = '\0';
    mock_connected = true;
    mock_force_error = ESP_OK;
}

static void record(const char *fmt, ...)
{
    size_t len = strlen(mock_calls);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mock_calls + len, sizeof(mock_calls) - len, fmt, ap);
    va_end(ap);
}

static esp_err_t outcome(void)
{
    if (!mock_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    return mock_force_error;
}

esp_err_t hid_device_init(void) { return ESP_OK; }
bool hid_is_connected(void) { return mock_connected; }

void hid_get_status(hid_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->connected = mock_connected;
    out->advertising = !mock_connected;
    out->link = mock_connected ? HID_LINK_CONNECTED : HID_LINK_ADVERTISING;
    out->bonded_peers = 1;
    out->reports_sent = 17;
    out->reports_failed = 2;
}

esp_err_t hid_move(int dx, int dy) { record("move(%d,%d);", dx, dy); return outcome(); }
esp_err_t hid_click(void) { record("click();"); return outcome(); }
esp_err_t hid_mouse_down(void) { record("down();"); return outcome(); }
esp_err_t hid_mouse_up(void) { record("up();"); return outcome(); }
esp_err_t hid_scroll(int dy) { record("scroll(%d);", dy); return outcome(); }
esp_err_t hid_mouse_report(uint8_t b, int8_t dx, int8_t dy, int8_t w) { record("mouse(%u,%d,%d,%d);", b, dx, dy, w); return outcome(); }
esp_err_t hid_key_press(hid_key_t k) { record("press(0x%02x,0x%02x);", k.keycode, k.modifiers); return outcome(); }
esp_err_t hid_key_down(hid_key_t k) { record("keydown(0x%02x,0x%02x);", k.keycode, k.modifiers); return outcome(); }
esp_err_t hid_key_up(hid_key_t k) { record("keyup(0x%02x,0x%02x);", k.keycode, k.modifiers); return outcome(); }
esp_err_t hid_release_all(void) { record("release_all();"); return outcome(); }
esp_err_t hid_unpair_all(void) { record("unpair();"); return mock_force_error; }

esp_err_t hid_type_text(const char *text)
{
    hid_key_t k;
    for (const char *p = text; *p; p++) {
        if (!hid_keymap_from_ascii(*p, &k)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    record("type(\"%s\");", text);
    return outcome();
}
