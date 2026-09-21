/*
 * Call audio between the Bluetooth audio link (8 kHz, 16-bit PCM, CVSD air mode) and the host.
 *
 *   phone -> call_audio_rx() -> RX ring -> call_audio_host_out() -> "A <base64 mu-law>" lines
 *   phone <- call_audio_tx() <- TX ring <- call_audio_host_in()  <- "A <base64 mu-law>" lines
 *
 * Test modes need no host: TONE sends a 400 Hz beep to the phone, LOOP sends the received
 * audio back after a delay. Pure C (no ESP-IDF, no RTOS) so the host tests cover it.
 *
 * Threading: each ring has one producer and one consumer.
 *   - call_audio_rx() and call_audio_tx() run in the Bluetooth audio callback (one task).
 *   - call_audio_host_in() runs in the console task.
 *   - call_audio_host_out() runs in the uplink task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CALL_AUDIO_SAMPLE_RATE    8000
#define CALL_AUDIO_FRAME_SAMPLES  160                   /* one host line = 20 ms */
#define CALL_AUDIO_LINE_MAX       240                   /* "A " + base64(160 bytes) + NUL */
#define CALL_AUDIO_HOST_IN_MAX    384                   /* most mu-law bytes accepted in one line */
#define CALL_AUDIO_LOOP_DELAY     4000                  /* LOOP mode echo delay, samples (0.5 s) */
#define CALL_AUDIO_STREAM_PREFILL 800                   /* STREAM mode jitter buffer, samples (0.1 s) */

typedef enum {
    CALL_AUDIO_OFF = 0,     /* send silence, keep the level meter */
    CALL_AUDIO_TONE,        /* send a 400 Hz beep (0.5 s on, 0.5 s off) */
    CALL_AUDIO_LOOP,        /* send the received audio back after CALL_AUDIO_LOOP_DELAY */
    CALL_AUDIO_STREAM,      /* exchange audio with the host */
} call_audio_mode_t;

typedef struct {
    call_audio_mode_t mode;
    bool link_open;
    uint32_t rx_frames;         /* frames received from the phone */
    uint32_t rx_bad_frames;     /* of those: marked bad by the baseband */
    uint32_t rx_dropped;        /* samples dropped because the host did not read fast enough */
    uint32_t tx_frames;         /* frames sent to the phone */
    uint32_t tx_underruns;      /* times the TX source ran dry while it was playing */
    uint32_t tx_dropped;        /* samples from the host dropped because the TX ring was full */
    uint32_t host_lines_in;     /* audio lines accepted from the host */
    uint32_t host_lines_bad;    /* audio lines rejected (bad base64, too long) */
    uint32_t host_lines_out;    /* audio lines sent to the host */
    uint16_t rx_peak;           /* largest |sample| received since the last statistics read */
} call_audio_stats_t;

void call_audio_init(void);

void call_audio_set_mode(call_audio_mode_t mode);
call_audio_mode_t call_audio_get_mode(void);
const char *call_audio_mode_name(call_audio_mode_t mode);

/** The Bluetooth audio link came up / went down. Open resets the counters and the rings. */
void call_audio_link_open(void);
void call_audio_link_close(void);

/** One frame from the phone: little-endian 16-bit PCM, bytes must be even. */
void call_audio_rx(const uint8_t *pcm_le, size_t bytes, bool bad_frame);

/** Fill the next frame for the phone: little-endian 16-bit PCM, bytes must be even. */
void call_audio_tx(uint8_t *pcm_le, size_t bytes);

/** One downlink line from the host (the base64 text after "A "). False if it was rejected. */
bool call_audio_host_in(const char *b64);

/**
 * @brief Build the next uplink line ("A <base64>", NUL-terminated) if a full frame is waiting.
 * @return length of the line, or 0 when there is nothing to send.
 */
size_t call_audio_host_out(char *line, size_t size);

/** Snapshot of the counters; reset_peak restarts the level meter. */
void call_audio_get_stats(call_audio_stats_t *out, bool reset_peak);

#ifdef __cplusplus
}
#endif
