#include <stdatomic.h>
#include <string.h>

#include "audio_codec.h"
#include "call_audio.h"

/* Ring sizes are powers of two (samples). RX: 0.5 s towards the host, TX: 1 s towards the phone. */
#define RX_RING_SAMPLES  4096
#define TX_RING_SAMPLES  8192
#define CHUNK_SAMPLES    64

/* Single-producer / single-consumer ring of samples with free-running counters. */
typedef struct {
    int16_t *buf;
    uint32_t mask;
    atomic_uint_least32_t head;     /* samples written, owned by the producer */
    atomic_uint_least32_t tail;     /* samples read, owned by the consumer */
} ring_t;

static int16_t s_rx_buf[RX_RING_SAMPLES];
static int16_t s_tx_buf[TX_RING_SAMPLES];
static ring_t s_rx = { .buf = s_rx_buf, .mask = RX_RING_SAMPLES - 1 };
static ring_t s_tx = { .buf = s_tx_buf, .mask = TX_RING_SAMPLES - 1 };

static atomic_int s_mode;
static atomic_bool s_link_open;
static atomic_bool s_rx_flush;      /* ask the RX consumer (uplink task) to empty the RX ring */
static atomic_bool s_tx_flush;      /* ask the TX consumer (Bluetooth side) to empty the TX ring */

/* Owned by the Bluetooth side (call_audio_tx). */
static bool s_tx_primed;
static uint32_t s_tone_pos;

static atomic_uint_least32_t s_rx_frames, s_rx_bad_frames, s_rx_dropped;
static atomic_uint_least32_t s_tx_frames, s_tx_underruns, s_tx_dropped;
static atomic_uint_least32_t s_host_lines_in, s_host_lines_bad, s_host_lines_out;
static atomic_uint_least32_t s_rx_peak;

/* 400 Hz at 8 kHz: 20 samples per period, amplitude 8000 (-12 dBFS). */
#define TONE_PERIOD      20
#define TONE_ON_SAMPLES  4000
#define TONE_CYCLE       8000
static const int16_t TONE_TABLE[TONE_PERIOD] = {
        0,  2472,  4702,  6472,  7608,  8000,  7608,  6472,  4702,  2472,
        0, -2472, -4702, -6472, -7608, -8000, -7608, -6472, -4702, -2472,
};

static uint32_t ring_fill(ring_t *r)
{
    return (uint32_t)(atomic_load(&r->head) - atomic_load(&r->tail));
}

/* Producer side. Writes what fits and returns that count; the caller counts the rest as dropped. */
static uint32_t ring_write(ring_t *r, const int16_t *src, uint32_t n)
{
    uint32_t head = atomic_load(&r->head);
    uint32_t space = (r->mask + 1) - (head - atomic_load(&r->tail));
    if (n > space) {
        n = space;
    }
    for (uint32_t i = 0; i < n; i++) {
        r->buf[(head + i) & r->mask] = src[i];
    }
    atomic_store(&r->head, head + n);
    return n;
}

/* Consumer side. Reads up to n samples and returns that count. */
static uint32_t ring_read(ring_t *r, int16_t *dst, uint32_t n)
{
    uint32_t tail = atomic_load(&r->tail);
    uint32_t fill = atomic_load(&r->head) - tail;
    if (n > fill) {
        n = fill;
    }
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = r->buf[(tail + i) & r->mask];
    }
    atomic_store(&r->tail, tail + n);
    return n;
}

/* Consumer side: drop everything that is waiting. */
static void ring_discard(ring_t *r)
{
    atomic_store(&r->tail, atomic_load(&r->head));
}

static void request_flush(void)
{
    atomic_store(&s_rx_flush, true);
    atomic_store(&s_tx_flush, true);
}

void call_audio_init(void)
{
    atomic_store(&s_mode, CALL_AUDIO_OFF);
    atomic_store(&s_link_open, false);
    atomic_store(&s_rx.head, 0);
    atomic_store(&s_rx.tail, 0);
    atomic_store(&s_tx.head, 0);
    atomic_store(&s_tx.tail, 0);
    atomic_store(&s_rx_flush, false);
    atomic_store(&s_tx_flush, false);
    s_tx_primed = false;
    s_tone_pos = 0;
    call_audio_link_close();
}

static void reset_counters(void)
{
    atomic_store(&s_rx_frames, 0);
    atomic_store(&s_rx_bad_frames, 0);
    atomic_store(&s_rx_dropped, 0);
    atomic_store(&s_tx_frames, 0);
    atomic_store(&s_tx_underruns, 0);
    atomic_store(&s_tx_dropped, 0);
    atomic_store(&s_host_lines_in, 0);
    atomic_store(&s_host_lines_bad, 0);
    atomic_store(&s_host_lines_out, 0);
    atomic_store(&s_rx_peak, 0);
}

void call_audio_set_mode(call_audio_mode_t mode)
{
    atomic_store(&s_mode, (int)mode);
    request_flush();
}

call_audio_mode_t call_audio_get_mode(void)
{
    return (call_audio_mode_t)atomic_load(&s_mode);
}

const char *call_audio_mode_name(call_audio_mode_t mode)
{
    switch (mode) {
    case CALL_AUDIO_TONE:   return "tone";
    case CALL_AUDIO_LOOP:   return "loop";
    case CALL_AUDIO_STREAM: return "stream";
    default:                return "off";
    }
}

void call_audio_link_open(void)
{
    reset_counters();
    request_flush();
    atomic_store(&s_link_open, true);
}

void call_audio_link_close(void)
{
    atomic_store(&s_link_open, false);
    request_flush();
}

/* Bluetooth side: apply a pending flush before this side touches the TX ring again. */
static void bt_side_sync(void)
{
    if (atomic_exchange(&s_tx_flush, false)) {
        ring_discard(&s_tx);
        s_tx_primed = false;
        s_tone_pos = 0;
    }
}

void call_audio_rx(const uint8_t *pcm_le, size_t bytes, bool bad_frame)
{
    if (!pcm_le || bytes < 2) {
        return;
    }
    bt_side_sync();
    atomic_fetch_add(&s_rx_frames, 1);
    if (bad_frame) {
        atomic_fetch_add(&s_rx_bad_frames, 1);
    }

    call_audio_mode_t mode = call_audio_get_mode();
    uint32_t peak = 0;
    size_t samples = bytes / 2;
    int16_t chunk[CHUNK_SAMPLES];
    uint32_t k = 0;

    for (size_t i = 0; i < samples; i++) {
        /* A bad frame keeps its place in time but carries no usable audio. */
        int16_t v = bad_frame ? 0 : (int16_t)(uint16_t)(pcm_le[2 * i] | ((uint16_t)pcm_le[2 * i + 1] << 8));
        uint32_t level = (uint32_t)(v < 0 ? -(int32_t)v : v);
        if (level > peak) {
            peak = level;
        }
        chunk[k++] = v;
        if (k == CHUNK_SAMPLES || i + 1 == samples) {
            if (mode == CALL_AUDIO_STREAM) {
                atomic_fetch_add(&s_rx_dropped, k - ring_write(&s_rx, chunk, k));
            } else if (mode == CALL_AUDIO_LOOP) {
                atomic_fetch_add(&s_tx_dropped, k - ring_write(&s_tx, chunk, k));
            }
            k = 0;
        }
    }

    if (peak > 32767) {
        peak = 32767;
    }
    if (peak > atomic_load(&s_rx_peak)) {
        atomic_store(&s_rx_peak, peak);
    }
}

static void put_le(uint8_t *pcm_le, size_t index, int16_t v)
{
    pcm_le[2 * index] = (uint8_t)((uint16_t)v & 0xFF);
    pcm_le[2 * index + 1] = (uint8_t)((uint16_t)v >> 8);
}

void call_audio_tx(uint8_t *pcm_le, size_t bytes)
{
    if (!pcm_le || bytes < 2) {
        return;
    }
    bt_side_sync();
    atomic_fetch_add(&s_tx_frames, 1);

    size_t samples = bytes / 2;
    size_t done = 0;
    call_audio_mode_t mode = call_audio_get_mode();

    if (mode == CALL_AUDIO_TONE) {
        for (; done < samples; done++) {
            int16_t v = s_tone_pos < TONE_ON_SAMPLES ? TONE_TABLE[s_tone_pos % TONE_PERIOD] : 0;
            s_tone_pos = (s_tone_pos + 1) % TONE_CYCLE;
            put_le(pcm_le, done, v);
        }
    } else if (mode == CALL_AUDIO_LOOP || mode == CALL_AUDIO_STREAM) {
        uint32_t prefill = (mode == CALL_AUDIO_LOOP) ? CALL_AUDIO_LOOP_DELAY : CALL_AUDIO_STREAM_PREFILL;
        if (!s_tx_primed && ring_fill(&s_tx) >= prefill) {
            s_tx_primed = true;
        }
        while (s_tx_primed && done < samples) {
            int16_t chunk[CHUNK_SAMPLES];
            uint32_t want = (uint32_t)(samples - done);
            if (want > CHUNK_SAMPLES) {
                want = CHUNK_SAMPLES;
            }
            uint32_t got = ring_read(&s_tx, chunk, want);
            for (uint32_t i = 0; i < got; i++) {
                put_le(pcm_le, done + i, chunk[i]);
            }
            done += got;
            if (got < want) {
                /* Ran dry: fill the rest with silence and wait for the prefill again. */
                s_tx_primed = false;
                atomic_fetch_add(&s_tx_underruns, 1);
            }
        }
    }

    for (; done < samples; done++) {
        put_le(pcm_le, done, 0);
    }
}

bool call_audio_host_in(const char *b64)
{
    uint8_t ulaw[CALL_AUDIO_HOST_IN_MAX];
    int n = b64 ? audio_b64_decode(b64, ulaw, sizeof(ulaw)) : -1;
    if (n <= 0) {
        atomic_fetch_add(&s_host_lines_bad, 1);
        return false;
    }
    atomic_fetch_add(&s_host_lines_in, 1);
    if (call_audio_get_mode() != CALL_AUDIO_STREAM) {
        return true;                                    /* accepted, but nobody listens */
    }

    int16_t chunk[CHUNK_SAMPLES];
    uint32_t k = 0;
    for (int i = 0; i < n; i++) {
        chunk[k++] = audio_ulaw_decode(ulaw[i]);
        if (k == CHUNK_SAMPLES || i + 1 == n) {
            atomic_fetch_add(&s_tx_dropped, k - ring_write(&s_tx, chunk, k));
            k = 0;
        }
    }
    return true;
}

size_t call_audio_host_out(char *line, size_t size)
{
    if (atomic_exchange(&s_rx_flush, false)) {
        ring_discard(&s_rx);
    }
    if (!line || size < CALL_AUDIO_LINE_MAX || ring_fill(&s_rx) < CALL_AUDIO_FRAME_SAMPLES) {
        return 0;
    }

    int16_t pcm[CALL_AUDIO_FRAME_SAMPLES];
    uint8_t ulaw[CALL_AUDIO_FRAME_SAMPLES];
    ring_read(&s_rx, pcm, CALL_AUDIO_FRAME_SAMPLES);
    for (size_t i = 0; i < CALL_AUDIO_FRAME_SAMPLES; i++) {
        ulaw[i] = audio_ulaw_encode(pcm[i]);
    }
    line[0] = 'A';
    line[1] = ' ';
    size_t n = audio_b64_encode(ulaw, sizeof(ulaw), line + 2, size - 2);
    atomic_fetch_add(&s_host_lines_out, 1);
    return n + 2;
}

void call_audio_get_stats(call_audio_stats_t *out, bool reset_peak)
{
    if (!out) {
        return;
    }
    out->mode = call_audio_get_mode();
    out->link_open = atomic_load(&s_link_open);
    out->rx_frames = atomic_load(&s_rx_frames);
    out->rx_bad_frames = atomic_load(&s_rx_bad_frames);
    out->rx_dropped = atomic_load(&s_rx_dropped);
    out->tx_frames = atomic_load(&s_tx_frames);
    out->tx_underruns = atomic_load(&s_tx_underruns);
    out->tx_dropped = atomic_load(&s_tx_dropped);
    out->host_lines_in = atomic_load(&s_host_lines_in);
    out->host_lines_bad = atomic_load(&s_host_lines_bad);
    out->host_lines_out = atomic_load(&s_host_lines_out);
    out->rx_peak = (uint16_t)(reset_peak ? atomic_exchange(&s_rx_peak, 0) : atomic_load(&s_rx_peak));
}
