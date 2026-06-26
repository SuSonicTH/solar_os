#include "battery_adc.h"

#include <math.h>
#include <stddef.h>

#include "esp_check.h"
#include "esp_log.h"
#include "adc_port.h"
#include "solar_os_board.h"

#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12
#define BATTERY_ADC_BITWIDTH ADC_BITWIDTH_12

static const char *TAG = "battery_adc";

esp_err_t battery_adc_init(void)
{
    adc_unit_t unit;
    adc_channel_t channel;
    ESP_RETURN_ON_ERROR(adc_port_configure_pin(SOLAR_OS_BOARD_PIN_BATTERY_ADC,
                                               BATTERY_ADC_ATTEN,
                                               BATTERY_ADC_BITWIDTH),
                        TAG,
                        "battery ADC config failed");
    ESP_RETURN_ON_FALSE(adc_port_is_adc_capable(SOLAR_OS_BOARD_PIN_BATTERY_ADC, &unit, &channel),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "battery ADC GPIO is not ADC capable");

    ESP_LOGI(TAG,
             "Battery ADC ready: GPIO=%d unit=%d channel=%d divider=%.2f",
             SOLAR_OS_BOARD_PIN_BATTERY_ADC,
             unit,
             channel,
             (double)SOLAR_OS_BOARD_BATTERY_ADC_DIVIDER_RATIO);
    return ESP_OK;
}

esp_err_t battery_adc_read(battery_adc_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    adc_port_sample_t adc_sample;
    ESP_RETURN_ON_ERROR(adc_port_read(SOLAR_OS_BOARD_PIN_BATTERY_ADC, &adc_sample),
                        TAG,
                        "battery ADC read failed");
    if (!adc_sample.calibrated) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int battery_mv =
        (int)lroundf((float)adc_sample.voltage_mv * SOLAR_OS_BOARD_BATTERY_ADC_DIVIDER_RATIO);

    sample->raw = adc_sample.raw;
    sample->adc_mv = adc_sample.voltage_mv;
    sample->battery_mv = (uint16_t)(battery_mv < 0 ? 0 : battery_mv);
    sample->calibrated = adc_sample.calibrated;
    return ESP_OK;
}
