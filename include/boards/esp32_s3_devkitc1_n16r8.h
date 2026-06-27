#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

#define SOLAR_OS_BOARD_ID "esp32_s3_devkitc1_n16r8"
#define SOLAR_OS_BOARD_NAME "Espressif ESP32-S3-DevKitC-1-N16R8"
#define SOLAR_OS_BOARD_VENDOR "Espressif"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-S3-WROOM-1-N16R8"

#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_43
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_44
