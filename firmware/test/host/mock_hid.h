#pragma once
#include <stdbool.h>
#include "esp_err.h"

extern char mock_calls[4096];
extern bool mock_connected;
extern esp_err_t mock_force_error;

void mock_reset(void);
