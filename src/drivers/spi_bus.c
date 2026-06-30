#include "spi_bus.h"

#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "solar_os_board.h"

#ifndef SOLAR_OS_BOARD_SPI_DMA_CH
#define SOLAR_OS_BOARD_SPI_DMA_CH SPI_DMA_CH_AUTO
#endif

#ifndef SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ
#define SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ 4096
#endif

static size_t spi_bus_ref_count;
static bool spi_bus_initialized_by_us;

spi_host_device_t solar_os_spi_bus_host(void)
{
    return SOLAR_OS_BOARD_SPI_HOST;
}

esp_err_t solar_os_spi_bus_acquire(void)
{
    if (spi_bus_ref_count > 0) {
        spi_bus_ref_count++;
        return ESP_OK;
    }

    const spi_bus_config_t bus_config = {
        .mosi_io_num = SOLAR_OS_BOARD_PIN_SPI_MOSI,
        .miso_io_num = SOLAR_OS_BOARD_PIN_SPI_MISO,
        .sclk_io_num = SOLAR_OS_BOARD_PIN_SPI_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ,
    };
    const esp_err_t err = spi_bus_initialize(SOLAR_OS_BOARD_SPI_HOST,
                                             &bus_config,
                                             SOLAR_OS_BOARD_SPI_DMA_CH);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    spi_bus_ref_count = 1;
    spi_bus_initialized_by_us = err == ESP_OK;
    return ESP_OK;
}

void solar_os_spi_bus_release(void)
{
    if (spi_bus_ref_count == 0) {
        return;
    }

    spi_bus_ref_count--;
    if (spi_bus_ref_count != 0) {
        return;
    }

    if (spi_bus_initialized_by_us) {
        (void)spi_bus_free(SOLAR_OS_BOARD_SPI_HOST);
    }
    spi_bus_initialized_by_us = false;
}
