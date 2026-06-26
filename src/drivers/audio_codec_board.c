#include "audio_codec_board.h"

#include <string.h>

#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "solar_os_board.h"

#define AUDIO_CODEC_I2S_MCLK_MULTIPLE I2S_MCLK_MULTIPLE_256
#define AUDIO_CODEC_TDM_SLOT_MASK \
    (I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3)

typedef struct {
    bool initialized;
    i2s_chan_handle_t tx_handle;
    i2s_chan_handle_t rx_handle;
    const audio_codec_data_if_t *data_if;
    const audio_codec_gpio_if_t *gpio_if;
    const audio_codec_ctrl_if_t *out_ctrl_if;
    const audio_codec_ctrl_if_t *in_ctrl_if;
    const audio_codec_if_t *out_codec_if;
    const audio_codec_if_t *in_codec_if;
    esp_codec_dev_handle_t playback;
    esp_codec_dev_handle_t record;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t volume;
    float mic_gain_db;
} audio_codec_board_state_t;

static const char *TAG = "audio_codec";
static audio_codec_board_state_t audio_codec;

static esp_err_t audio_codec_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SOLAR_OS_BOARD_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &audio_codec.tx_handle, &audio_codec.rx_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    i2s_tdm_config_t tdm_cfg = {
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(32,
                                                        I2S_SLOT_MODE_STEREO,
                                                        AUDIO_CODEC_TDM_SLOT_MASK),
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE),
        .gpio_cfg = {
            .mclk = SOLAR_OS_BOARD_PIN_I2S_MCLK,
            .bclk = SOLAR_OS_BOARD_PIN_I2S_BCLK,
            .ws = SOLAR_OS_BOARD_PIN_I2S_WS,
            .dout = SOLAR_OS_BOARD_PIN_I2S_DOUT,
            .din = SOLAR_OS_BOARD_PIN_I2S_DIN,
        },
    };
    tdm_cfg.slot_cfg.total_slot = 4;
    tdm_cfg.clk_cfg.mclk_multiple = AUDIO_CODEC_I2S_MCLK_MULTIPLE;

    ret = i2s_channel_init_tdm_mode(audio_codec.tx_handle, &tdm_cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = i2s_channel_init_tdm_mode(audio_codec.rx_handle, &tdm_cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = i2s_channel_enable(audio_codec.tx_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    return i2s_channel_enable(audio_codec.rx_handle);
}

static esp_err_t audio_codec_create_devices(void)
{
    i2c_master_bus_handle_t i2c_handle = i2c_bus_get_handle();
    if (i2c_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        return ESP_ERR_NO_MEM;
    }
    audio_codec.gpio_if = gpio_if;

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = SOLAR_OS_BOARD_I2S_PORT,
        .rx_handle = audio_codec.rx_handle,
        .tx_handle = audio_codec.tx_handle,
    };
    audio_codec.data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (audio_codec.data_if == NULL) {
        return ESP_ERR_NO_MEM;
    }

    audio_codec_i2c_cfg_t out_i2c = {
        .port = SOLAR_OS_BOARD_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    audio_codec.out_ctrl_if = audio_codec_new_i2c_ctrl(&out_i2c);
    if (audio_codec.out_ctrl_if == NULL) {
        return ESP_ERR_NO_MEM;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .ctrl_if = audio_codec.out_ctrl_if,
        .gpio_if = audio_codec.gpio_if,
        .pa_pin = SOLAR_OS_BOARD_PIN_AUDIO_PA,
        .use_mclk = true,
        .hw_gain.pa_gain = 6.0f,
    };
    audio_codec.out_codec_if = es8311_codec_new(&es8311_cfg);
    if (audio_codec.out_codec_if == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_cfg_t playback_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = audio_codec.out_codec_if,
        .data_if = audio_codec.data_if,
    };
    audio_codec.playback = esp_codec_dev_new(&playback_cfg);
    if (audio_codec.playback == NULL) {
        return ESP_ERR_NO_MEM;
    }

    audio_codec_i2c_cfg_t in_i2c = {
        .port = SOLAR_OS_BOARD_I2C_PORT,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    audio_codec.in_ctrl_if = audio_codec_new_i2c_ctrl(&in_i2c);
    if (audio_codec.in_ctrl_if == NULL) {
        return ESP_ERR_NO_MEM;
    }

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = audio_codec.in_ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    audio_codec.in_codec_if = es7210_codec_new(&es7210_cfg);
    if (audio_codec.in_codec_if == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_cfg_t record_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = audio_codec.in_codec_if,
        .data_if = audio_codec.data_if,
    };
    audio_codec.record = esp_codec_dev_new(&record_cfg);
    if (audio_codec.record == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t audio_codec_open_devices(void)
{
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE,
        .channel = AUDIO_CODEC_BOARD_DEFAULT_CHANNELS,
        .bits_per_sample = AUDIO_CODEC_BOARD_DEFAULT_BITS,
    };

    if (esp_codec_dev_open(audio_codec.playback, &sample_info) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    if (esp_codec_dev_open(audio_codec.record, &sample_info) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_out_vol(audio_codec.playback,
                                  AUDIO_CODEC_BOARD_DEFAULT_VOLUME) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_in_gain(audio_codec.record,
                                  AUDIO_CODEC_BOARD_DEFAULT_MIC_GAIN_DB) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }

    audio_codec.sample_rate = AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE;
    audio_codec.channels = AUDIO_CODEC_BOARD_DEFAULT_CHANNELS;
    audio_codec.bits_per_sample = AUDIO_CODEC_BOARD_DEFAULT_BITS;
    audio_codec.volume = AUDIO_CODEC_BOARD_DEFAULT_VOLUME;
    audio_codec.mic_gain_db = AUDIO_CODEC_BOARD_DEFAULT_MIC_GAIN_DB;
    return ESP_OK;
}

esp_err_t audio_codec_board_init(void)
{
    if (audio_codec.initialized) {
        return ESP_OK;
    }

    esp_err_t ret = i2c_bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_codec_i2s_init();
    if (ret == ESP_OK) {
        i2c_bus_lock();
        ret = audio_codec_create_devices();
        if (ret == ESP_OK) {
            ret = audio_codec_open_devices();
        }
        i2c_bus_unlock();
    }

    if (ret != ESP_OK) {
        audio_codec_board_deinit();
        return ret;
    }

    audio_codec.initialized = true;
    ESP_LOGI(TAG,
             "audio ready: %s/%s I2S%d mclk=%d bclk=%d ws=%d din=%d dout=%d pa=%d",
             SOLAR_OS_BOARD_AUDIO_CODEC_OUT,
             SOLAR_OS_BOARD_AUDIO_CODEC_IN,
             (int)SOLAR_OS_BOARD_I2S_PORT,
             (int)SOLAR_OS_BOARD_PIN_I2S_MCLK,
             (int)SOLAR_OS_BOARD_PIN_I2S_BCLK,
             (int)SOLAR_OS_BOARD_PIN_I2S_WS,
             (int)SOLAR_OS_BOARD_PIN_I2S_DIN,
             (int)SOLAR_OS_BOARD_PIN_I2S_DOUT,
             (int)SOLAR_OS_BOARD_PIN_AUDIO_PA);
    return ESP_OK;
}

void audio_codec_board_deinit(void)
{
    if (audio_codec.playback != NULL) {
        esp_codec_dev_close(audio_codec.playback);
        esp_codec_dev_delete(audio_codec.playback);
    }
    if (audio_codec.record != NULL) {
        esp_codec_dev_close(audio_codec.record);
        esp_codec_dev_delete(audio_codec.record);
    }
    if (audio_codec.in_codec_if != NULL) {
        audio_codec_delete_codec_if(audio_codec.in_codec_if);
    }
    if (audio_codec.out_codec_if != NULL) {
        audio_codec_delete_codec_if(audio_codec.out_codec_if);
    }
    if (audio_codec.in_ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(audio_codec.in_ctrl_if);
    }
    if (audio_codec.out_ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(audio_codec.out_ctrl_if);
    }
    if (audio_codec.data_if != NULL) {
        audio_codec_delete_data_if(audio_codec.data_if);
    }
    if (audio_codec.gpio_if != NULL) {
        audio_codec_delete_gpio_if(audio_codec.gpio_if);
    }
    if (audio_codec.tx_handle != NULL) {
        i2s_channel_disable(audio_codec.tx_handle);
        i2s_del_channel(audio_codec.tx_handle);
    }
    if (audio_codec.rx_handle != NULL) {
        i2s_channel_disable(audio_codec.rx_handle);
        i2s_del_channel(audio_codec.rx_handle);
    }

    memset(&audio_codec, 0, sizeof(audio_codec));
}

esp_err_t audio_codec_board_set_volume(uint8_t volume)
{
    if (volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = audio_codec_board_init();
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_bus_lock();
    ret = esp_codec_dev_set_out_vol(audio_codec.playback, volume) == ESP_CODEC_DEV_OK ?
        ESP_OK :
        ESP_FAIL;
    i2c_bus_unlock();
    if (ret == ESP_OK) {
        audio_codec.volume = volume;
    }
    return ret;
}

esp_err_t audio_codec_board_set_mic_gain(float gain_db)
{
    esp_err_t ret = audio_codec_board_init();
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_bus_lock();
    ret = esp_codec_dev_set_in_gain(audio_codec.record, gain_db) == ESP_CODEC_DEV_OK ?
        ESP_OK :
        ESP_FAIL;
    i2c_bus_unlock();
    if (ret == ESP_OK) {
        audio_codec.mic_gain_db = gain_db;
    }
    return ret;
}

esp_err_t audio_codec_board_write(const void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_codec_board_init();
    if (ret != ESP_OK) {
        return ret;
    }
    return esp_codec_dev_write(audio_codec.playback, (void *)data, (int)len) == ESP_CODEC_DEV_OK ?
        ESP_OK :
        ESP_FAIL;
}

esp_err_t audio_codec_board_read(void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_codec_board_init();
    if (ret != ESP_OK) {
        return ret;
    }
    return esp_codec_dev_read(audio_codec.record, data, (int)len) == ESP_CODEC_DEV_OK ?
        ESP_OK :
        ESP_FAIL;
}

void audio_codec_board_get_status(audio_codec_board_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->initialized = audio_codec.initialized;
    status->sample_rate = audio_codec.sample_rate != 0 ?
        audio_codec.sample_rate :
        AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE;
    status->channels = audio_codec.channels != 0 ?
        audio_codec.channels :
        AUDIO_CODEC_BOARD_DEFAULT_CHANNELS;
    status->bits_per_sample = audio_codec.bits_per_sample != 0 ?
        audio_codec.bits_per_sample :
        AUDIO_CODEC_BOARD_DEFAULT_BITS;
    status->volume = audio_codec.initialized ? audio_codec.volume : AUDIO_CODEC_BOARD_DEFAULT_VOLUME;
    status->mic_gain_db = audio_codec.initialized ?
        audio_codec.mic_gain_db :
        AUDIO_CODEC_BOARD_DEFAULT_MIC_GAIN_DB;
    status->i2s_port = SOLAR_OS_BOARD_I2S_PORT;
    status->mclk_pin = SOLAR_OS_BOARD_PIN_I2S_MCLK;
    status->bclk_pin = SOLAR_OS_BOARD_PIN_I2S_BCLK;
    status->ws_pin = SOLAR_OS_BOARD_PIN_I2S_WS;
    status->din_pin = SOLAR_OS_BOARD_PIN_I2S_DIN;
    status->dout_pin = SOLAR_OS_BOARD_PIN_I2S_DOUT;
    status->pa_pin = SOLAR_OS_BOARD_PIN_AUDIO_PA;
    status->output_codec = SOLAR_OS_BOARD_AUDIO_CODEC_OUT;
    status->input_codec = SOLAR_OS_BOARD_AUDIO_CODEC_IN;
}
