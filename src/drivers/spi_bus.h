#pragma once

#include "driver/spi_master.h"
#include "esp_err.h"

esp_err_t solar_os_spi_bus_acquire(void);
void solar_os_spi_bus_release(void);
spi_host_device_t solar_os_spi_bus_host(void);
