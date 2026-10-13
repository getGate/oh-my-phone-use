#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "hid_device.h"
#include "input_player.h"

static const char *TAG = "player";

#define PLAYER_TASK_STACK    4096
#define PLAYER_TASK_PRIO     5
#define STOP_ACK_TIMEOUT_MS  1500
#define WAIT_SLICE_MS        20

static struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t stopped;          /* given by the task after it has stopped a running batch */
    TaskHandle_t task;
    input_record_t records[INPUT_RECORDS_MAX];
    size_t count;
    size_t cursor;
    bool running;
    volatile bool stop_requested;
    input_player_state_t state;
    int64_t deadline_us;
    uint32_t wait_ms;
    uint32_t batches;
} s_player;

static void lock(void)
{
    xSemaphoreTake(s_player.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_player.lock);
}

const char *input_player_state_name(input_player_state_t state)
{
    switch (state) {
    case INPUT_PLAYER_QUEUED:  return "queued";
    case INPUT_PLAYER_TYPING:  return "typing";
    case INPUT_PLAYER_DONE:    return "done";
    case INPUT_PLAYER_STOPPED: return "stopped";
    default:                   return "idle";
    }
}

/* Every hid_* call already sleeps CONFIG_PHONEUSE_HID_REPORT_INTERVAL_MS after its report;
 * only add what is missing to reach the requested spacing. */
static void delay_after_report(uint32_t target_ms)
{
    uint32_t extra = target_ms > CONFIG_PHONEUSE_HID_REPORT_INTERVAL_MS
                     ? target_ms - CONFIG_PHONEUSE_HID_REPORT_INTERVAL_MS : 0;
    if (extra) {
        vTaskDelay(pdMS_TO_TICKS(extra));
    }
}

static bool past_deadline(void)
{
    return esp_timer_get_time() >= s_player.deadline_us;
}

/* Sleep in short slices so POST /stop and the deadline are honoured during long waits. */
static void wait_interruptible(uint32_t ms)
{
    while (ms > 0 && !s_player.stop_requested && !past_deadline()) {
        uint32_t slice = ms < WAIT_SLICE_MS ? ms : WAIT_SLICE_MS;
        lock();
        s_player.wait_ms = ms;
        unlock();
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
    }
    lock();
    s_player.wait_ms = 0;
    unlock();
}

static esp_err_t play_record(const input_record_t *r)
{
    switch (r->type) {
    case INPUT_RECORD_KEY: {
        hid_key_t key;
        if (!input_record_key(r, &key)) {
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t err = hid_key_down(key);
        if (err != ESP_OK) {
            return err;
        }
        delay_after_report(CONFIG_PHONEUSE_INPUT_TOOL_KEY_HOLD_MS);
        err = hid_key_up(key);
        delay_after_report(CONFIG_PHONEUSE_INPUT_TOOL_RECORD_GAP_MS);
        return err;
    }
    case INPUT_RECORD_MOUSE: {
        esp_err_t err = hid_mouse_report(r->a, (int8_t)r->b, (int8_t)r->c, (int8_t)r->d);
        delay_after_report(CONFIG_PHONEUSE_INPUT_TOOL_RECORD_GAP_MS);
        return err;
    }
    case INPUT_RECORD_WAIT:
        wait_interruptible(input_record_wait_ms(r));
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static void run_batch(void)
{
    lock();
    s_player.state = INPUT_PLAYER_TYPING;
    size_t count = s_player.count;
    unlock();

    esp_err_t err = ESP_OK;
    size_t cursor = 0;
    while (cursor < count && !s_player.stop_requested) {
        if (past_deadline()) {
            ESP_LOGW(TAG, "batch deadline reached after %u of %u records", (unsigned)cursor, (unsigned)count);
            err = ESP_ERR_TIMEOUT;
            break;
        }
        err = play_record(&s_player.records[cursor]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "record %u (type %u) failed: %s", (unsigned)cursor,
                     s_player.records[cursor].type, esp_err_to_name(err));
            break;
        }
        cursor++;
        lock();
        s_player.cursor = cursor;
        unlock();
    }

    bool completed = (err == ESP_OK && cursor == count && !s_player.stop_requested);
    if (!completed) {
        /* Nothing may stay pressed after an interrupted batch. */
        hid_release_all();
    }
    lock();
    s_player.running = false;
    s_player.wait_ms = 0;
    s_player.state = completed ? INPUT_PLAYER_DONE : INPUT_PLAYER_STOPPED;
    bool ack = s_player.stop_requested;
    s_player.stop_requested = false;
    unlock();
    if (ack) {
        xSemaphoreGive(s_player.stopped);
    }
    ESP_LOGI(TAG, "batch %s (%u/%u records)", completed ? "done" : "stopped", (unsigned)cursor, (unsigned)count);
}

static void player_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        run_batch();
    }
}

esp_err_t input_player_init(void)
{
    if (s_player.task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_player.lock = xSemaphoreCreateMutex();
    s_player.stopped = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_player.lock && s_player.stopped, ESP_ERR_NO_MEM, TAG, "sync objects");
    BaseType_t ok = xTaskCreate(player_task, "input_player", PLAYER_TASK_STACK, NULL, PLAYER_TASK_PRIO, &s_player.task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

esp_err_t input_player_submit(const input_record_t *records, size_t count)
{
    if (!s_player.task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!records || count == 0 || count > INPUT_RECORDS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    if (s_player.running) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_player.records, records, count * sizeof(*records));
    s_player.count = count;
    s_player.cursor = 0;
    s_player.running = true;
    s_player.stop_requested = false;
    s_player.state = INPUT_PLAYER_QUEUED;
    s_player.wait_ms = 0;
    s_player.deadline_us = esp_timer_get_time() + (int64_t)CONFIG_PHONEUSE_INPUT_TOOL_DEADLINE_S * 1000000LL;
    s_player.batches++;
    unlock();
    xTaskNotifyGive(s_player.task);
    return ESP_OK;
}

void input_player_stop(void)
{
    if (!s_player.task) {
        hid_release_all();
        return;
    }
    lock();
    bool was_running = s_player.running;
    if (was_running) {
        xSemaphoreTake(s_player.stopped, 0);        /* drop a stale acknowledgement */
        s_player.stop_requested = true;
    } else {
        s_player.state = INPUT_PLAYER_STOPPED;
    }
    unlock();

    if (was_running) {
        if (xSemaphoreTake(s_player.stopped, pdMS_TO_TICKS(STOP_ACK_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "stop requested; player has not acknowledged yet");
        }
    } else {
        /* Same as the reference firmware: a stop always releases everything. */
        hid_release_all();
    }
}

void input_player_get_status(input_player_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_player.lock) {
        return;
    }
    lock();
    out->running = s_player.running;
    out->state = s_player.state;
    out->wait_ms = s_player.wait_ms;
    out->cursor = s_player.cursor;
    out->count = s_player.count;
    out->batches = s_player.batches;
    unlock();
}
