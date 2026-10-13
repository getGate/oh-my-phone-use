/*
 * Splits a large relative movement into a sequence of per-report deltas that each fit the
 * HID report range (|delta| <= max_step). Pure C, unit-tested on the host.
 *
 * The steps follow a straight line (cumulative rounding, like Bresenham) and always sum to
 * exactly the requested movement.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t target_x, target_y;
    int32_t sent_x, sent_y;
    int32_t steps, index;
} hid_motion_t;

/** Initialise an iterator. Returns the number of reports needed (0 for a zero move). */
int32_t hid_motion_begin(hid_motion_t *m, int32_t dx, int32_t dy, int32_t max_step);

/** Produce the next delta. Returns false when the movement is complete. */
bool hid_motion_next(hid_motion_t *m, int8_t *step_x, int8_t *step_y);

#ifdef __cplusplus
}
#endif
