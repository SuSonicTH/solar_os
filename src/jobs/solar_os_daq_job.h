#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"
#include "solar_os_storage.h"
#include "solar_os_stream.h"

#define SOLAR_OS_DAQ_STREAM_LIST_MAX 160

typedef struct {
    bool running;
    char stream_id[SOLAR_OS_STREAM_ID_MAX];
    char stream_ids[SOLAR_OS_DAQ_STREAM_LIST_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solar_os_stream_type_t stream_type;
    size_t stream_count;
    uint32_t interval_ms;
    uint32_t written_records;
    uint64_t written_bytes;
    uint32_t skipped_records;
    uint32_t failed_records;
    bool change_only;
    bool append;
    bool raw;
    esp_err_t last_error;
} solar_os_daq_status_t;

extern const solar_os_job_t solar_os_daq_job;

void solar_os_daq_job_get_status(solar_os_daq_status_t *status);
