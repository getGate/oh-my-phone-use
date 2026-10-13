/*
 * Plays validated input-record batches back through the logical HID API from its own task, so
 * the HTTP server stays responsive (and POST /stop works) while a batch is running.
 *
 * States mirror the reference firmware: idle -> queued -> typing -> done, or stopped (by
 * POST /stop, the batch deadline, a lost Bluetooth link or a refused report).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "input_records.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INPUT_PLAYER_IDLE = 0,
    INPUT_PLAYER_QUEUED,
    INPUT_PLAYER_TYPING,
    INPUT_PLAYER_DONE,
    INPUT_PLAYER_STOPPED,
} input_player_state_t;

typedef struct {
    bool running;
    input_player_state_t state;
    uint32_t wait_ms;       /*!< remaining time of the wait record being executed, else 0 */
    size_t cursor;          /*!< records executed so far in the current/last batch */
    size_t count;           /*!< records in the current/last batch */
    uint32_t batches;       /*!< batches accepted since boot */
} input_player_status_t;

esp_err_t input_player_init(void);

/**
 * @brief Queue a batch. The records must already have passed input_records_validate().
 * @return ESP_ERR_INVALID_STATE while another batch is running.
 */
esp_err_t input_player_submit(const input_record_t *records, size_t count);

/**
 * @brief Release every key and button and stop the running batch (state becomes "stopped",
 *        even when nothing was running). Waits briefly for the player task to acknowledge.
 */
void input_player_stop(void);

void input_player_get_status(input_player_status_t *out);
const char *input_player_state_name(input_player_state_t state);

#ifdef __cplusplus
}
#endif
