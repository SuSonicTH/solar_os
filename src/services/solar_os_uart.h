#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_port.h"

#define SOLAR_OS_UART_DEFAULT_BAUD_RATE 115200U
#define SOLAR_OS_UART_MIN_BAUD_RATE 300U
#define SOLAR_OS_UART_MAX_BAUD_RATE 921600U
#define SOLAR_OS_UART_PORT_NAME "uart0"

typedef enum {
    SOLAR_OS_UART_MODE_RAW,
    SOLAR_OS_UART_MODE_LINE,
} solar_os_uart_mode_t;

typedef struct {
    bool initialized;
    int port_num;
    int tx_pin;
    int rx_pin;
    uint32_t baud_rate;
    solar_os_uart_mode_t mode;
    size_t rx_buffered;
    bool rx_buffered_valid;
    bool port_claimed;
    char port_owner[SOLAR_OS_PORT_OWNER_MAX];
} solar_os_uart_status_t;

esp_err_t solar_os_uart_init(void);
bool solar_os_uart_is_valid_baud_rate(uint32_t baud_rate);
esp_err_t solar_os_uart_set_baud_rate(uint32_t baud_rate);
esp_err_t solar_os_uart_set_mode(solar_os_uart_mode_t mode);
esp_err_t solar_os_uart_write(const uint8_t *data, size_t len, size_t *written);
esp_err_t solar_os_uart_read(uint8_t *data, size_t len, uint32_t timeout_ms, size_t *read_len);
void solar_os_uart_get_status(solar_os_uart_status_t *status);
const char *solar_os_uart_mode_name(solar_os_uart_mode_t mode);
bool solar_os_uart_parse_mode(const char *text, solar_os_uart_mode_t *mode);
