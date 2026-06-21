#pragma once

#include <stddef.h>

void *solar_os_psram_malloc(size_t size);
void *solar_os_psram_calloc(size_t count, size_t size);
