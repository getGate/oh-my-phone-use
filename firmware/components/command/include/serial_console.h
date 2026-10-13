/*
 * Serial transport for the command protocol: reads lines from the console UART (UART0, the
 * board's USB-UART bridge), runs them through command_parser and writes the response.
 * Also routes ESP-IDF log output through the same UART driver so log lines and protocol
 * responses never interleave mid-line.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t serial_console_start(void);

/**
 * @brief Send one unsolicited line to the host (an event or call audio); the newline is added.
 *        Lines longer than COMMAND_RESPONSE_MAX are cut. Safe from any task, not from an ISR.
 */
void serial_console_write_line(const char *line);

#ifdef __cplusplus
}
#endif
