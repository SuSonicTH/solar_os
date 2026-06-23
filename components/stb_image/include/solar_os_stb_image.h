#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t solar_os_stb_decode_gray(const uint8_t *data,
                                   size_t len,
                                   uint32_t max_pixels,
                                   uint8_t **out_gray,
                                   uint32_t *out_width,
                                   uint32_t *out_height);
esp_err_t solar_os_stb_jpeg_decode_gray(const uint8_t *data,
                                         size_t len,
                                         uint32_t max_pixels,
                                         uint8_t **out_gray,
                                         uint32_t *out_width,
                                         uint32_t *out_height);
void solar_os_stb_image_free(void *data);
const char *solar_os_stb_failure_reason(void);

#ifdef __cplusplus
}
#endif
