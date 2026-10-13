#include <stdbool.h>
#include <string.h>

#include "audio_codec.h"

/* ---- G.711 mu-law ---------------------------------------------------------------------- */

#define ULAW_BIAS  0x84
#define ULAW_CLIP  32635

uint8_t audio_ulaw_encode(int16_t pcm)
{
    int sample = pcm;
    int sign = (sample >> 8) & 0x80;
    if (sign) {
        sample = -sample;
    }
    if (sample > ULAW_CLIP) {
        sample = ULAW_CLIP;
    }
    sample += ULAW_BIAS;

    int exponent = 7;
    for (int mask = 0x4000; (sample & mask) == 0 && exponent > 0; mask >>= 1) {
        exponent--;
    }
    int mantissa = (sample >> (exponent + 3)) & 0x0F;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

int16_t audio_ulaw_decode(uint8_t ulaw)
{
    ulaw = (uint8_t)~ulaw;
    int exponent = (ulaw >> 4) & 0x07;
    int mantissa = ulaw & 0x0F;
    int sample = (((mantissa << 3) + ULAW_BIAS) << exponent) - ULAW_BIAS;
    return (int16_t)((ulaw & 0x80) ? -sample : sample);
}

/* ---- base64 ---------------------------------------------------------------------------- */

static const char B64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t audio_b64_encode(const uint8_t *in, size_t len, char *out, size_t out_size)
{
    size_t need = AUDIO_B64_ENCODED_LEN(len);
    if (!in || !out || len == 0 || out_size < need + 1) {
        return 0;
    }
    char *w = out;
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        *w++ = B64_ALPHABET[(v >> 18) & 0x3F];
        *w++ = B64_ALPHABET[(v >> 12) & 0x3F];
        *w++ = B64_ALPHABET[(v >> 6) & 0x3F];
        *w++ = B64_ALPHABET[v & 0x3F];
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        *w++ = B64_ALPHABET[(v >> 18) & 0x3F];
        *w++ = B64_ALPHABET[(v >> 12) & 0x3F];
        *w++ = (i + 1 < len) ? B64_ALPHABET[(v >> 6) & 0x3F] : '=';
        *w++ = '=';
    }
    *w = '\0';
    return (size_t)(w - out);
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

int audio_b64_decode(const char *in, uint8_t *out, size_t out_size)
{
    if (!in || !out) {
        return -1;
    }
    size_t len = strlen(in);
    if (len == 0 || (len % 4) != 0) {
        return -1;
    }
    size_t pad = 0;
    if (in[len - 1] == '=') {
        pad++;
        if (in[len - 2] == '=') {
            pad++;
        }
    }
    size_t bytes = (len / 4) * 3 - pad;
    if (bytes > out_size) {
        return -1;
    }

    size_t w = 0;
    for (size_t i = 0; i < len; i += 4) {
        bool last = (i + 4 == len);
        uint32_t v = 0;
        for (size_t k = 0; k < 4; k++) {
            char c = in[i + k];
            int d;
            if (c == '=' && last && k >= 4 - pad) {
                d = 0;
            } else if ((d = b64_value(c)) < 0) {
                return -1;
            }
            v = (v << 6) | (uint32_t)d;
        }
        size_t group = last ? 3 - pad : 3;
        for (size_t k = 0; k < group; k++) {
            out[w++] = (uint8_t)(v >> (16 - 8 * k));
        }
    }
    return (int)w;
}
