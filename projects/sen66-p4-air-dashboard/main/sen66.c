#include "sen66.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define SEN66_ADDRESS 0x6B
#define SEN66_START_MEASUREMENT 0x0021
#define SEN66_READ_VALUES 0x0300

static i2c_master_dev_handle_t sensor;
static const char *TAG = "sen66";

static uint8_t crc8(const uint8_t *bytes, size_t length)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t command(uint16_t value)
{
    const uint8_t bytes[] = {(uint8_t)(value >> 8), (uint8_t)value};
    return i2c_master_transmit(sensor, bytes, sizeof(bytes), 100);
}

esp_err_t sen66_init(i2c_master_bus_handle_t bus)
{
    if (!bus) return ESP_ERR_INVALID_ARG;

    bool found = false;
    ESP_LOGI(TAG, "Scanning I2C bus (SDA GPIO7, SCL GPIO8)...");
    for (uint8_t address = 0x08; address < 0x78; ++address) {
        if (i2c_master_probe(bus, address, 50) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X%s", address,
                     address == SEN66_ADDRESS ? " (SEN66)" : "");
            found |= address == SEN66_ADDRESS;
        }
    }
    if (!found) ESP_LOGW(TAG, "No SEN66 probe response at 0x%02X; trying start command anyway", SEN66_ADDRESS);

    esp_err_t err = ESP_OK;
    if (!sensor) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = SEN66_ADDRESS,
            .scl_speed_hz = 100000,
        };
        err = i2c_master_bus_add_device(bus, &config, &sensor);
        if (err != ESP_OK) return err;
    }
    for (int attempt = 1; attempt <= 3; ++attempt) {
        err = command(SEN66_START_MEASUREMENT);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "Start attempt %d failed: %s", attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(1200));
    return err;
}

esp_err_t sen66_read(sen66_data_t *data)
{
    if (!sensor || !data) return ESP_ERR_INVALID_STATE;
    const uint8_t request[] = {(uint8_t)(SEN66_READ_VALUES >> 8), (uint8_t)SEN66_READ_VALUES};
    uint8_t response[27];
    esp_err_t err = i2c_master_transmit(sensor, request, sizeof(request), 100);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));
    err = i2c_master_receive(sensor, response, sizeof(response), 100);
    if (err != ESP_OK) return err;

    uint16_t word[9];
    for (int i = 0; i < 9; ++i) {
        if (crc8(&response[i * 3], 2) != response[i * 3 + 2]) return ESP_ERR_INVALID_CRC;
        word[i] = ((uint16_t)response[i * 3] << 8) | response[i * 3 + 1];
    }
    data->pm1 = word[0] == UINT16_MAX ? NAN : word[0] / 10.0f;
    data->pm25 = word[1] == UINT16_MAX ? NAN : word[1] / 10.0f;
    data->pm4 = word[2] == UINT16_MAX ? NAN : word[2] / 10.0f;
    data->pm10 = word[3] == UINT16_MAX ? NAN : word[3] / 10.0f;
    data->humidity = word[4] == 0x7FFF ? NAN : (int16_t)word[4] / 100.0f;
    data->temperature = word[5] == 0x7FFF ? NAN : (int16_t)word[5] / 200.0f;
    data->voc = word[6] == 0x7FFF ? NAN : (int16_t)word[6] / 10.0f;
    data->nox = word[7] == 0x7FFF ? NAN : (int16_t)word[7] / 10.0f;
    data->co2 = word[8] == UINT16_MAX ? NAN : word[8];
    return ESP_OK;
}

bool sen66_data_valid(const sen66_data_t *data)
{
    return data && (!isnan(data->co2) || !isnan(data->pm25));
}
