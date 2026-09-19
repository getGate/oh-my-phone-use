#include "hid_motion.h"

static int32_t iabs32(int32_t v)
{
    return v < 0 ? -v : v;
}

static int32_t ceil_div(int32_t a, int32_t b) /* a >= 0, b > 0 */
{
    return (a + b - 1) / b;
}

int32_t hid_motion_begin(hid_motion_t *m, int32_t dx, int32_t dy, int32_t max_step)
{
    if (max_step < 1) {
        max_step = 1;
    } else if (max_step > 127) {
        max_step = 127;
    }
    m->target_x = dx;
    m->target_y = dy;
    m->sent_x = 0;
    m->sent_y = 0;
    m->index = 0;

    int32_t nx = ceil_div(iabs32(dx), max_step);
    int32_t ny = ceil_div(iabs32(dy), max_step);
    m->steps = nx > ny ? nx : ny;
    return m->steps;
}

bool hid_motion_next(hid_motion_t *m, int8_t *step_x, int8_t *step_y)
{
    if (m->index >= m->steps) {
        return false;
    }
    m->index++;
    /* Cumulative position after this step, truncated toward zero. Consecutive differences are
     * bounded by ceil(|target| / steps) <= max_step, and the last step lands exactly on target. */
    int64_t cx = ((int64_t)m->target_x * m->index) / m->steps;
    int64_t cy = ((int64_t)m->target_y * m->index) / m->steps;
    *step_x = (int8_t)(cx - m->sent_x);
    *step_y = (int8_t)(cy - m->sent_y);
    m->sent_x = (int32_t)cx;
    m->sent_y = (int32_t)cy;
    return true;
}
