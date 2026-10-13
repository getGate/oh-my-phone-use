/*
 * The input-device secret: value of the X-Input-Device-Secret header that every mutating HTTP
 * request must carry. Sources, in order of precedence: NVS (set with the SECRET serial
 * command, survives reflashing) then the build-time CONFIG_PHONEUSE_INPUT_TOOL_SECRET.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_SECRET_MAX_LEN 128

esp_err_t input_secret_init(void);
bool input_secret_configured(void);
const char *input_secret_source(void);              /* "nvs", "build" or "none" */
bool input_secret_matches(const char *candidate);   /* constant-time comparison */
/** Store a new secret in NVS; NULL or "" removes the stored one (falls back to the build value). */
esp_err_t input_secret_set(const char *secret);

#ifdef __cplusplus
}
#endif
