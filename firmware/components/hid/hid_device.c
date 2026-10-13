/*
 * Logical HID layer: explicit input state (held buttons, held keys), report construction,
 * movement splitting and text typing. Talks to the Bluetooth transport only through
 * hid_transport.h.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "hid_device.h"
#include "hid_keymap.h"
#include "hid_motion.h"
#include "hid_transport.h"

static const char *TAG = "hid";

#define KEY_SLOTS 6

static struct {
    SemaphoreHandle_t lock;
    bool initialised;
    uint8_t buttons;            /* held mouse buttons, sent with every mouse report */
    uint8_t modifiers;          /* held modifier bits */
    uint8_t keys[KEY_SLOTS];    /* held key codes (0 = empty slot) */
    uint32_t reports_sent;
    uint32_t reports_failed;
} s_hid;

/* ---- helpers (call with the lock held) ------------------------------------------------ */

static void report_gap(void)
{
#if CONFIG_PHONEUSE_HID_REPORT_INTERVAL_MS > 0
    TickType_t ticks = pdMS_TO_TICKS(CONFIG_PHONEUSE_HID_REPORT_INTERVAL_MS);
    vTaskDelay(ticks ? ticks : 1);
#endif
}

static esp_err_t send_report(uint8_t report_id, const uint8_t *data, size_t len)
{
    esp_err_t err = hid_transport_send_input(report_id, data, len);
    if (err == ESP_OK) {
        s_hid.reports_sent++;
    } else {
        s_hid.reports_failed++;
        ESP_LOGW(TAG, "report id %u not sent: %s", report_id, esp_err_to_name(err));
    }
    report_gap();
    return err;
}

static esp_err_t send_mouse(int8_t dx, int8_t dy, int8_t wheel)
{
    uint8_t report[HID_MOUSE_REPORT_LEN] = {
        s_hid.buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel, 0 /* horizontal pan */
    };
    return send_report(HID_REPORT_ID_MOUSE, report, sizeof(report));
}

static esp_err_t send_keyboard(void)
{
    uint8_t report[HID_KEYBOARD_REPORT_LEN] = { s_hid.modifiers, 0 };
    memcpy(&report[2], s_hid.keys, KEY_SLOTS);
    return send_report(HID_REPORT_ID_KEYBOARD, report, sizeof(report));
}

static esp_err_t require_ready(void)
{
    if (!s_hid.initialised || !hid_transport_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void lock(void)
{
    xSemaphoreTake(s_hid.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_hid.lock);
}

/* ---- lifecycle / status --------------------------------------------------------------- */

esp_err_t hid_device_init(void)
{
    if (s_hid.initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    s_hid.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_hid.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex");
    ESP_RETURN_ON_ERROR(hid_transport_init(), TAG, "transport init failed");
    s_hid.initialised = true;
    return ESP_OK;
}

void hid_get_status(hid_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    hid_transport_link_t link = { 0 };
    if (s_hid.initialised) {
        hid_transport_get_link(&link);
    }
    out->connected = link.connected;
    out->advertising = link.advertising;
    out->bonded_peers = link.bonded_peers;
    out->link = link.connected ? HID_LINK_CONNECTED : (link.advertising ? HID_LINK_ADVERTISING : HID_LINK_DOWN);
    out->mouse_buttons = s_hid.buttons;
    out->reports_sent = s_hid.reports_sent;
    out->reports_failed = s_hid.reports_failed;
}

bool hid_is_connected(void)
{
    return s_hid.initialised && hid_transport_connected();
}

/* ---- mouse ---------------------------------------------------------------------------- */

esp_err_t hid_move(int dx, int dy)
{
    if (dx < -HID_MAX_DELTA || dx > HID_MAX_DELTA || dy < -HID_MAX_DELTA || dy > HID_MAX_DELTA) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = require_ready();
    if (err == ESP_OK) {
        hid_motion_t motion;
        int8_t sx, sy;
        hid_motion_begin(&motion, dx, dy, CONFIG_PHONEUSE_HID_MOUSE_MAX_STEP);
        while (err == ESP_OK && hid_motion_next(&motion, &sx, &sy)) {
            err = send_mouse(sx, sy, 0);
        }
    }
    unlock();
    return err;
}

esp_err_t hid_scroll(int dy)
{
    if (dy < -HID_MAX_DELTA || dy > HID_MAX_DELTA) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = require_ready();
    if (err == ESP_OK) {
        hid_motion_t motion;
        int8_t wheel, unused;
        hid_motion_begin(&motion, dy, 0, CONFIG_PHONEUSE_HID_MOUSE_MAX_STEP);
        while (err == ESP_OK && hid_motion_next(&motion, &wheel, &unused)) {
            err = send_mouse(0, 0, wheel);
        }
    }
    unlock();
    return err;
}

esp_err_t hid_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (buttons & (uint8_t)~0x1F) {             /* the report map has 5 button bits */
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = require_ready();
    if (err == ESP_OK) {
        uint8_t previous = s_hid.buttons;
        s_hid.buttons = buttons;
        err = send_mouse(dx, dy, wheel);
        if (err != ESP_OK) {
            s_hid.buttons = previous;           /* state reflects what the host actually saw */
        }
    }
    unlock();
    return err;
}

static esp_err_t set_buttons(uint8_t buttons)
{
    if (buttons == s_hid.buttons) {
        return ESP_OK;
    }
    uint8_t previous = s_hid.buttons;
    s_hid.buttons = buttons;
    esp_err_t err = send_mouse(0, 0, 0);
    if (err != ESP_OK) {
        s_hid.buttons = previous;   /* state reflects what the host actually saw */
    }
    return err;
}

esp_err_t hid_click(void)
{
    lock();
    esp_err_t err = require_ready();
    if (err == ESP_OK) {
        err = set_buttons(s_hid.buttons | HID_MOUSE_BUTTON_LEFT);
    }
    if (err == ESP_OK) {
        err = set_buttons(s_hid.buttons & (uint8_t)~HID_MOUSE_BUTTON_LEFT);
    }
    unlock();
    return err;
}

esp_err_t hid_mouse_down(void)
{
    lock();
    esp_err_t err = require_ready();
    if (err == ESP_OK) {
        err = set_buttons(s_hid.buttons | HID_MOUSE_BUTTON_LEFT);
    }
    unlock();
    return err;
}

esp_err_t hid_mouse_up(void)
{
    lock();
    esp_err_t err = require_ready();
    if (err == ESP_OK) {
        err = set_buttons(s_hid.buttons & (uint8_t)~HID_MOUSE_BUTTON_LEFT);
    }
    unlock();
    return err;
}

/* ---- keyboard ------------------------------------------------------------------------- */

static esp_err_t key_down_locked(hid_key_t key)
{
    esp_err_t err = require_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (key.keycode) {
        int slot = -1;
        for (int i = 0; i < KEY_SLOTS; i++) {
            if (s_hid.keys[i] == key.keycode) {
                slot = i;                       /* already held */
                break;
            }
            if (s_hid.keys[i] == 0 && slot < 0) {
                slot = i;
            }
        }
        if (slot < 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        s_hid.keys[slot] = key.keycode;
    }
    s_hid.modifiers |= key.modifiers;
    return send_keyboard();
}

static esp_err_t key_up_locked(hid_key_t key)
{
    esp_err_t err = require_ready();
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < KEY_SLOTS; i++) {
        if (key.keycode && s_hid.keys[i] == key.keycode) {
            s_hid.keys[i] = 0;
        }
    }
    s_hid.modifiers &= (uint8_t)~key.modifiers;
    return send_keyboard();
}

esp_err_t hid_key_down(hid_key_t key)
{
    lock();
    esp_err_t err = key_down_locked(key);
    unlock();
    return err;
}

esp_err_t hid_key_up(hid_key_t key)
{
    lock();
    esp_err_t err = key_up_locked(key);
    unlock();
    return err;
}

esp_err_t hid_key_press(hid_key_t key)
{
    lock();
    esp_err_t err = key_down_locked(key);
    if (err == ESP_OK) {
        err = key_up_locked(key);
    }
    unlock();
    return err;
}

esp_err_t hid_release_all(void)
{
    lock();
    esp_err_t err = require_ready();
    if (err == ESP_OK) {
        memset(s_hid.keys, 0, sizeof(s_hid.keys));
        s_hid.modifiers = 0;
        err = send_keyboard();
        esp_err_t err2 = set_buttons(0);
        if (err == ESP_OK) {
            err = err2;
        }
    }
    unlock();
    return err;
}

esp_err_t hid_type_text(const char *text)
{
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Validate first so an untypeable character never leaves the text half typed. */
    hid_key_t key;
    for (const char *p = text; *p; p++) {
        if (!hid_keymap_from_ascii(*p, &key)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    lock();
    esp_err_t err = require_ready();
    for (const char *p = text; err == ESP_OK && *p; p++) {
        hid_keymap_from_ascii(*p, &key);
        /* One key at a time: press report, then an all-released report (so "ll" types twice). */
        memset(s_hid.keys, 0, sizeof(s_hid.keys));
        s_hid.keys[0] = key.keycode;
        s_hid.modifiers = key.modifiers;
        err = send_keyboard();
        if (err == ESP_OK) {
            s_hid.keys[0] = 0;
            s_hid.modifiers = 0;
            err = send_keyboard();
        }
    }
    unlock();
    return err;
}

/* ---- pairing -------------------------------------------------------------------------- */

esp_err_t hid_unpair_all(void)
{
    if (!s_hid.initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    return hid_transport_unpair_all();
}
