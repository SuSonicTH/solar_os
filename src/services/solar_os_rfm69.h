#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

esp_err_t solar_os_rfm69_attach(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count);
void solar_os_rfm69_detach(const char *name);

