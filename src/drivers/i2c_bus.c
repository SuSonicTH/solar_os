#include "i2c_bus.h"

#include <inttypes.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_board.h"

#define I2C_XFER_TIMEOUT_MS 100

static const char *TAG = "i2c_bus";

static i2c_master_bus_handle_t bus_handle;
static SemaphoreHandle_t bus_mutex;

static esp_err_t i2c_bus_device(uint8_t address, i2c_master_dev_handle_t *dev_handle)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = SOLAR_I2C_SPEED_HZ,
    };

    return i2c_master_bus_add_device(bus_handle, &dev_config, dev_handle);
}

esp_err_t i2c_bus_init(void)
{
    if (bus_handle != NULL) {
        return ESP_OK;
    }

    bus_mutex = xSemaphoreCreateMutex();
    if (bus_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = SOLAR_OS_BOARD_I2C_PORT,
        .sda_io_num = SOLAR_OS_BOARD_PIN_I2C_SDA,
        .scl_io_num = SOLAR_OS_BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus_handle), TAG, "new I2C bus failed");
    ESP_LOGI(TAG,
             "I2C bus ready: port=%d SDA=%d SCL=%d speed=%" PRIu32,
             (int)SOLAR_OS_BOARD_I2C_PORT,
             SOLAR_OS_BOARD_PIN_I2C_SDA,
             SOLAR_OS_BOARD_PIN_I2C_SCL,
             SOLAR_I2C_SPEED_HZ);

    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return bus_handle;
}

void i2c_bus_lock(void)
{
    if (bus_mutex != NULL) {
        xSemaphoreTake(bus_mutex, portMAX_DELAY);
    }
}

void i2c_bus_unlock(void)
{
    if (bus_mutex != NULL) {
        xSemaphoreGive(bus_mutex);
    }
}

uint32_t i2c_bus_get_speed_hz(void)
{
    return SOLAR_I2C_SPEED_HZ;
}

gpio_num_t i2c_bus_get_sda_pin(void)
{
    return SOLAR_OS_BOARD_PIN_I2C_SDA;
}

gpio_num_t i2c_bus_get_scl_pin(void)
{
    return SOLAR_OS_BOARD_PIN_I2C_SCL;
}

esp_err_t i2c_bus_probe(uint8_t address)
{
    if (bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(bus_mutex, portMAX_DELAY);
    const esp_err_t ret = i2c_master_probe(bus_handle, address, I2C_XFER_TIMEOUT_MS);
    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_transmit(uint8_t address, const uint8_t *data, size_t len)
{
    if (bus_handle == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_bus_device(address, &dev_handle);
    if (ret == ESP_OK) {
        ret = i2c_master_transmit(dev_handle, data, len, I2C_XFER_TIMEOUT_MS);
        esp_err_t rm_ret = i2c_master_bus_rm_device(dev_handle);
        if (ret == ESP_OK) {
            ret = rm_ret;
        }
    }

    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_receive(uint8_t address, uint8_t *data, size_t len)
{
    if (bus_handle == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_bus_device(address, &dev_handle);
    if (ret == ESP_OK) {
        ret = i2c_master_receive(dev_handle, data, len, I2C_XFER_TIMEOUT_MS);
        esp_err_t rm_ret = i2c_master_bus_rm_device(dev_handle);
        if (ret == ESP_OK) {
            ret = rm_ret;
        }
    }

    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_transmit_receive(uint8_t address,
                                   const uint8_t *tx_data,
                                   size_t tx_len,
                                   uint8_t *rx_data,
                                   size_t rx_len)
{
    if (bus_handle == NULL || tx_data == NULL || tx_len == 0 || rx_data == NULL || rx_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_bus_device(address, &dev_handle);
    if (ret == ESP_OK) {
        ret = i2c_master_transmit_receive(dev_handle, tx_data, tx_len, rx_data, rx_len, I2C_XFER_TIMEOUT_MS);
        esp_err_t rm_ret = i2c_master_bus_rm_device(dev_handle);
        if (ret == ESP_OK) {
            ret = rm_ret;
        }
    }

    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_read_reg(uint8_t address, uint8_t reg, uint8_t *data, size_t len)
{
    if (bus_handle == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_bus_device(address, &dev_handle);
    if (ret == ESP_OK) {
        ret = i2c_master_transmit_receive(dev_handle, &reg, 1, data, len, I2C_XFER_TIMEOUT_MS);
        esp_err_t rm_ret = i2c_master_bus_rm_device(dev_handle);
        if (ret == ESP_OK) {
            ret = rm_ret;
        }
    }

    xSemaphoreGive(bus_mutex);
    return ret;
}

esp_err_t i2c_bus_write_reg(uint8_t address, uint8_t reg, const uint8_t *data, size_t len)
{
    if (bus_handle == NULL || data == NULL || len == 0 || len > 31) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buffer[32];
    buffer[0] = reg;
    for (size_t i = 0; i < len; i++) {
        buffer[i + 1] = data[i];
    }

    xSemaphoreTake(bus_mutex, portMAX_DELAY);

    i2c_master_dev_handle_t dev_handle;
    esp_err_t ret = i2c_bus_device(address, &dev_handle);
    if (ret == ESP_OK) {
        ret = i2c_master_transmit(dev_handle, buffer, len + 1, I2C_XFER_TIMEOUT_MS);
        esp_err_t rm_ret = i2c_master_bus_rm_device(dev_handle);
        if (ret == ESP_OK) {
            ret = rm_ret;
        }
    }

    xSemaphoreGive(bus_mutex);
    return ret;
}
