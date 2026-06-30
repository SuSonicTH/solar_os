set(SOLAR_OS_BOARD_STORAGE_DRIVER "sdspi")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_storage_sd.c"
    "drivers/spi_bus.c"
    "drivers/sd_card.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_gpio
    esp_driver_sdspi
    esp_driver_spi
    fatfs
    sdmmc
)
