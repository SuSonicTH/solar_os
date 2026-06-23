#include "solar_os_stb_image.h"

#include <limits.h>
#include <string.h>

#include "esp_heap_caps.h"

static void *solar_os_stbi_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *solar_os_stbi_realloc_sized(void *ptr, size_t old_size, size_t new_size)
{
    if (new_size == 0) {
        heap_caps_free(ptr);
        return NULL;
    }

    void *new_ptr = solar_os_stbi_malloc(new_size);
    if (new_ptr == NULL) {
        return NULL;
    }
    if (new_ptr != NULL && ptr != NULL) {
        memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    }
    heap_caps_free(ptr);
    return new_ptr;
}

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_MALLOC(sz) solar_os_stbi_malloc(sz)
#define STBI_REALLOC_SIZED(p, oldsz, newsz) solar_os_stbi_realloc_sized(p, oldsz, newsz)
#define STBI_FREE(p) heap_caps_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

esp_err_t solar_os_stb_decode_gray(const uint8_t *data,
                                   size_t len,
                                   uint32_t max_pixels,
                                   uint8_t **out_gray,
                                   uint32_t *out_width,
                                   uint32_t *out_height)
{
    if (data == NULL || len == 0 || out_gray == NULL || out_width == NULL || out_height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_gray = NULL;
    *out_width = 0;
    *out_height = 0;

    int width = 0;
    int height = 0;
    int channels = 0;
    if (!stbi_info_from_memory(data, (int)len, &width, &height, &channels)) {
        return ESP_FAIL;
    }
    if (width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint64_t pixels = (uint64_t)width * (uint64_t)height;
    if (pixels > SIZE_MAX || (max_pixels != 0 && pixels > max_pixels)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *gray = stbi_load_from_memory(data, (int)len, &width, &height, &channels, 1);
    if (gray == NULL) {
        return ESP_FAIL;
    }

    *out_gray = gray;
    *out_width = (uint32_t)width;
    *out_height = (uint32_t)height;
    return ESP_OK;
}

esp_err_t solar_os_stb_jpeg_decode_gray(const uint8_t *data,
                                         size_t len,
                                         uint32_t max_pixels,
                                         uint8_t **out_gray,
                                         uint32_t *out_width,
                                         uint32_t *out_height)
{
    return solar_os_stb_decode_gray(data,
                                    len,
                                    max_pixels,
                                    out_gray,
                                    out_width,
                                    out_height);
}

void solar_os_stb_image_free(void *data)
{
    stbi_image_free(data);
}

const char *solar_os_stb_failure_reason(void)
{
    const char *reason = stbi_failure_reason();
    return reason != NULL ? reason : "unknown";
}
