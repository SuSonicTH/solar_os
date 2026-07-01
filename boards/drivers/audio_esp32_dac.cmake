set(SOLAR_OS_BOARD_AUDIO_DRIVER "esp32_dac")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_audio_dac.c"
    "drivers/audio_dac_board.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_dac
)
