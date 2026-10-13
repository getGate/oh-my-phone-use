#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "command_parser.h"
#include "serial_console.h"

static const char *TAG = "console";

#define CONSOLE_UART        UART_NUM_0
#define CONSOLE_BAUD        CONFIG_ESP_CONSOLE_UART_BAUDRATE
#define CONSOLE_RX_BUF      2048
#define CONSOLE_TX_BUF      4096
#define CONSOLE_TASK_STACK  5120
#define CONSOLE_TASK_PRIO   5
#define LOG_LINE_MAX        256

static SemaphoreHandle_t s_log_lock;

/*
 * Log sink: assemble complete lines before handing them to the UART driver, so a log line
 * emitted by another task can never be spliced into the middle of a protocol response.
 */
static int console_log_vprintf(const char *fmt, va_list ap)
{
    static char line[LOG_LINE_MAX];
    static size_t fill;
    char chunk[LOG_LINE_MAX];

    int n = vsnprintf(chunk, sizeof(chunk), fmt, ap);
    if (n <= 0) {
        return n;
    }
    size_t len = (size_t)n < sizeof(chunk) ? (size_t)n : sizeof(chunk) - 1;

    if (s_log_lock && xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (size_t i = 0; i < len; i++) {
            line[fill++] = chunk[i];
            if (chunk[i] == '\n' || fill == sizeof(line)) {
                uart_write_bytes(CONSOLE_UART, line, fill);
                fill = 0;
            }
        }
        xSemaphoreGive(s_log_lock);
    } else {
        uart_write_bytes(CONSOLE_UART, chunk, len);
    }
    return n;
}

void serial_console_write_line(const char *line)
{
    /* One driver call per line: uart_write_bytes() is atomic, so lines of other tasks cannot split it. */
    char out[COMMAND_RESPONSE_MAX + 1];
    size_t n = strnlen(line, COMMAND_RESPONSE_MAX);
    memcpy(out, line, n);
    out[n++] = '\n';
    uart_write_bytes(CONSOLE_UART, out, n);
}

static void handle_line(const char *line)
{
    char response[COMMAND_RESPONSE_MAX + 1];
    size_t n = command_parser_execute(line, response, COMMAND_RESPONSE_MAX);
    if (n == 0) {
        return;
    }
    response[n++] = '\n';
    uart_write_bytes(CONSOLE_UART, response, n);
}

static void console_task(void *arg)
{
    static char line[COMMAND_LINE_MAX];
    size_t fill = 0;
    bool overflow = false;
    uint8_t rx[64];

    for (;;) {
        int n = uart_read_bytes(CONSOLE_UART, rx, sizeof(rx), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)rx[i];
            if (c == '\n') {
                if (overflow) {
                    uart_write_bytes(CONSOLE_UART, "ERR line_too_long\n", 18);
                    overflow = false;
                } else {
                    line[fill] = '\0';
                    handle_line(line);
                }
                fill = 0;
            } else if (fill < sizeof(line) - 1) {
                line[fill++] = c;
            } else {
                overflow = true;
            }
        }
    }
}

esp_err_t serial_console_start(void)
{
    const uart_config_t cfg = {
        .baud_rate = CONSOLE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(CONSOLE_UART, CONSOLE_RX_BUF, CONSOLE_TX_BUF, 0, NULL, 0),
                        TAG, "uart driver");
    ESP_RETURN_ON_ERROR(uart_param_config(CONSOLE_UART, &cfg), TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(CONSOLE_UART, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pins");

    /* stdout/stdin now go through the driver too (no direct FIFO writes racing the driver). */
    uart_vfs_dev_use_driver(CONSOLE_UART);

    s_log_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_log_lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex");
    esp_log_set_vprintf(console_log_vprintf);

    BaseType_t ok = xTaskCreate(console_task, "console", CONSOLE_TASK_STACK, NULL, CONSOLE_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");

    ESP_LOGI(TAG, "serial command console on UART%d at %d baud", CONSOLE_UART, CONSOLE_BAUD);
    return ESP_OK;
}
