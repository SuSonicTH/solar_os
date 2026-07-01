#include "solar_os_board_audio.h"

#include "audio_dac_board.h"

static void audio_status_from_driver(solar_os_board_audio_status_t *out,
                                     const audio_dac_board_status_t *in)
{
    out->initialized = in->initialized;
    out->sample_rate = in->sample_rate;
    out->channels = in->channels;
    out->bits_per_sample = in->bits_per_sample;
    out->volume = in->volume;
    out->mic_gain_db = 0.0f;
    out->i2s_port = -1;
    out->mclk_pin = -1;
    out->bclk_pin = -1;
    out->ws_pin = -1;
    out->din_pin = in->dac_neg_pin;
    out->dout_pin = in->dac_pos_pin;
    out->pa_pin = -1;
    out->output_codec = in->output_codec;
    out->input_codec = in->input_codec;
}

esp_err_t solar_os_board_audio_init(void)
{
    return audio_dac_board_init();
}

void solar_os_board_audio_deinit(void)
{
    audio_dac_board_deinit();
}

esp_err_t solar_os_board_audio_set_volume(uint8_t volume)
{
    return audio_dac_board_set_volume(volume);
}

esp_err_t solar_os_board_audio_set_mic_gain(float gain_db)
{
    return audio_dac_board_set_mic_gain(gain_db);
}

esp_err_t solar_os_board_audio_write(const void *data, size_t len)
{
    return audio_dac_board_write(data, len);
}

esp_err_t solar_os_board_audio_read(void *data, size_t len)
{
    return audio_dac_board_read(data, len);
}

void solar_os_board_audio_get_status(solar_os_board_audio_status_t *status)
{
    if (status == NULL) {
        return;
    }

    audio_dac_board_status_t driver_status;
    audio_dac_board_get_status(&driver_status);
    audio_status_from_driver(status, &driver_status);
}
