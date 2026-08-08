#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    float pm1;
    float pm25;
    float pm4;
    float pm10;
    float humidity;
    float temperature;
    float voc;
    float nox;
    float co2;
} sen66_data_t;

esp_err_t sen66_init(i2c_master_bus_handle_t bus);
esp_err_t sen66_read(sen66_data_t *data);
bool sen66_data_valid(const sen66_data_t *data);
