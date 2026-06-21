#include "solar_os_memory.h"

#include "esp_heap_caps.h"

void *solar_os_psram_malloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

void *solar_os_psram_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0) {
        return NULL;
    }

    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}
