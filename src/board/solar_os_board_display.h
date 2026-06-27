#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "u8g2.h"

typedef struct {
    void *driver;
    u8g2_t *u8g2;
    const char *controller;
    uint16_t width;
    uint16_t height;
    bool ready;
} solar_os_board_display_t;

esp_err_t solar_os_board_display_init(solar_os_board_display_t *display);
esp_err_t solar_os_board_display_resume(solar_os_board_display_t *display);
void solar_os_board_display_deinit(solar_os_board_display_t *display);
u8g2_t *solar_os_board_display_u8g2(solar_os_board_display_t *display);
const char *solar_os_board_display_controller(const solar_os_board_display_t *display);
uint16_t solar_os_board_display_width(const solar_os_board_display_t *display);
uint16_t solar_os_board_display_height(const solar_os_board_display_t *display);
bool solar_os_board_display_ready(const solar_os_board_display_t *display);
