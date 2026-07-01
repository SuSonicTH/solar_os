#include "audio_dac_board.h"

#include <string.h>

#include "driver/dac_continuous.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "solar_os_board.h"

#define AUDIO_DAC_DESC_NUM 8U
#define AUDIO_DAC_DMA_BUFFER_BYTES 1024U
#define AUDIO_DAC_CONVERT_BUFFER_BYTES 2048U
#define AUDIO_DAC_INPUT_FRAME_BYTES \
    ((AUDIO_DAC_BOARD_DEFAULT_CHANNELS * AUDIO_DAC_BOARD_DEFAULT_BITS) / 8U)
#define AUDIO_DAC_WRITE_TIMEOUT_MS 1000
#define AUDIO_DAC_MIDPOINT 128U

typedef struct {
    bool initialized;
    bool volume_set;
    dac_continuous_handle_t handle;
    uint8_t *buffer;
    uint8_t volume;
} audio_dac_board_state_t;

static const char *TAG = "audio_dac";
static audio_dac_board_state_t audio_dac;

static uint8_t audio_dac_current_volume(void)
{
    if (!audio_dac.volume_set) {
        return AUDIO_DAC_BOARD_DEFAULT_VOLUME;
    }
    return audio_dac.volume;
}

static uint8_t audio_dac_sample_to_u8(int32_t sample)
{
    if (sample < -128) {
        sample = -128;
    } else if (sample > 127) {
        sample = 127;
    }
    return (uint8_t)(sample + (int32_t)AUDIO_DAC_MIDPOINT);
}

static size_t audio_dac_convert_frames(const int16_t *input, size_t frames, uint8_t *output)
{
    const uint8_t volume = audio_dac_current_volume();

    for (size_t frame = 0; frame < frames; frame++) {
        const int32_t left = input[(frame * AUDIO_DAC_BOARD_DEFAULT_CHANNELS) + 0];
        const int32_t right = input[(frame * AUDIO_DAC_BOARD_DEFAULT_CHANNELS) + 1];
        int32_t mixed = (left + right) / 2;
        mixed = (mixed * (int32_t)volume) / 100;
        const int32_t sample8 = mixed >> 8;

        output[(frame * 2U) + 0U] = audio_dac_sample_to_u8(sample8);
        output[(frame * 2U) + 1U] = audio_dac_sample_to_u8(-sample8);
    }

    return frames * 2U;
}

esp_err_t audio_dac_board_init(void)
{
#if !SOC_DAC_SUPPORTED
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (audio_dac.initialized) {
        return ESP_OK;
    }

    if (!audio_dac.volume_set) {
        audio_dac.volume = AUDIO_DAC_BOARD_DEFAULT_VOLUME;
        audio_dac.volume_set = true;
    }

    audio_dac.buffer = heap_caps_malloc(AUDIO_DAC_CONVERT_BUFFER_BYTES,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(audio_dac.buffer != NULL, ESP_ERR_NO_MEM, TAG, "no DAC buffer");

    dac_continuous_config_t config = {
        .chan_mask = DAC_CHANNEL_MASK_ALL,
        .desc_num = AUDIO_DAC_DESC_NUM,
        .buf_size = AUDIO_DAC_DMA_BUFFER_BYTES,
        .freq_hz = AUDIO_DAC_BOARD_DEFAULT_SAMPLE_RATE * 2U,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode = DAC_CHANNEL_MODE_ALTER,
    };

    esp_err_t ret = dac_continuous_new_channels(&config, &audio_dac.handle);
    if (ret == ESP_OK) {
        ret = dac_continuous_enable(audio_dac.handle);
    }
    if (ret != ESP_OK) {
        audio_dac_board_deinit();
        return ret;
    }

    memset(audio_dac.buffer, AUDIO_DAC_MIDPOINT, AUDIO_DAC_CONVERT_BUFFER_BYTES);
    (void)dac_continuous_write(audio_dac.handle,
                               audio_dac.buffer,
                               AUDIO_DAC_DMA_BUFFER_BYTES,
                               NULL,
                               AUDIO_DAC_WRITE_TIMEOUT_MS);

    audio_dac.initialized = true;
    ESP_LOGI(TAG,
             "audio ready: %s differential dac pos=%d neg=%d rate=%u volume=%u",
             SOLAR_OS_BOARD_AUDIO_CODEC_OUT,
             (int)SOLAR_OS_BOARD_PIN_AUDIO_DAC_POS,
             (int)SOLAR_OS_BOARD_PIN_AUDIO_DAC_NEG,
             AUDIO_DAC_BOARD_DEFAULT_SAMPLE_RATE,
             (unsigned)audio_dac.volume);
    return ESP_OK;
#endif
}

void audio_dac_board_deinit(void)
{
#if SOC_DAC_SUPPORTED
    if (audio_dac.handle != NULL) {
        if (audio_dac.buffer != NULL) {
            memset(audio_dac.buffer, AUDIO_DAC_MIDPOINT, AUDIO_DAC_DMA_BUFFER_BYTES);
            (void)dac_continuous_write(audio_dac.handle,
                                       audio_dac.buffer,
                                       AUDIO_DAC_DMA_BUFFER_BYTES,
                                       NULL,
                                       50);
        }
        (void)dac_continuous_disable(audio_dac.handle);
        (void)dac_continuous_del_channels(audio_dac.handle);
    }
#endif
    if (audio_dac.buffer != NULL) {
        heap_caps_free(audio_dac.buffer);
    }
    audio_dac.handle = NULL;
    audio_dac.buffer = NULL;
    audio_dac.initialized = false;
}

esp_err_t audio_dac_board_set_volume(uint8_t volume)
{
    if (volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_dac.volume = volume;
    audio_dac.volume_set = true;
    return audio_dac_board_init();
}

esp_err_t audio_dac_board_set_mic_gain(float gain_db)
{
    (void)gain_db;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_dac_board_write(const void *data, size_t len)
{
    if (data == NULL || len == 0 || (len % AUDIO_DAC_INPUT_FRAME_BYTES) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_dac_board_init();
    if (ret != ESP_OK) {
        return ret;
    }

    const int16_t *input = (const int16_t *)data;
    size_t frames_remaining = len / AUDIO_DAC_INPUT_FRAME_BYTES;
    const size_t frames_per_chunk = AUDIO_DAC_CONVERT_BUFFER_BYTES / 2U;

    while (frames_remaining > 0) {
        const size_t frames = frames_remaining > frames_per_chunk ?
            frames_per_chunk :
            frames_remaining;
        const size_t output_bytes = audio_dac_convert_frames(input, frames, audio_dac.buffer);
        ret = dac_continuous_write(audio_dac.handle,
                                   audio_dac.buffer,
                                   output_bytes,
                                   NULL,
                                   AUDIO_DAC_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            return ret;
        }
        input += frames * AUDIO_DAC_BOARD_DEFAULT_CHANNELS;
        frames_remaining -= frames;
    }

    return ESP_OK;
}

esp_err_t audio_dac_board_read(void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_dac_board_get_status(audio_dac_board_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->initialized = audio_dac.initialized;
    status->sample_rate = AUDIO_DAC_BOARD_DEFAULT_SAMPLE_RATE;
    status->channels = AUDIO_DAC_BOARD_DEFAULT_CHANNELS;
    status->bits_per_sample = AUDIO_DAC_BOARD_DEFAULT_BITS;
    status->volume = audio_dac_current_volume();
    status->dac_pos_pin = SOLAR_OS_BOARD_PIN_AUDIO_DAC_POS;
    status->dac_neg_pin = SOLAR_OS_BOARD_PIN_AUDIO_DAC_NEG;
    status->output_codec = SOLAR_OS_BOARD_AUDIO_CODEC_OUT;
    status->input_codec = SOLAR_OS_BOARD_AUDIO_CODEC_IN;
}
