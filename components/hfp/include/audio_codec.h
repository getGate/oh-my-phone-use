/*
 * Small codecs for the call audio that goes over the serial line: G.711 mu-law (16-bit PCM
 * <-> 8 bits per sample) and base64 (bytes <-> printable text). Pure C, host-tested.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t audio_ulaw_encode(int16_t pcm);
int16_t audio_ulaw_decode(uint8_t ulaw);

/** Length of the base64 text for len bytes, without the terminating NUL. */
#define AUDIO_B64_ENCODED_LEN(len)  ((((len) + 2) / 3) * 4)

/**
 * @brief Encode len bytes as padded base64 and NUL-terminate the text.
 * @return number of characters written, or 0 if out_size is too small (or len is 0).
 */
size_t audio_b64_encode(const uint8_t *in, size_t len, char *out, size_t out_size);

/**
 * @brief Decode padded base64 text (standard alphabet, no whitespace).
 * @return number of bytes written, or -1 on invalid text or if out_size is too small.
 */
int audio_b64_decode(const char *in, uint8_t *out, size_t out_size);

#ifdef __cplusplus
}
#endif
