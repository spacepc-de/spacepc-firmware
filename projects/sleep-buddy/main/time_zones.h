#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

const char *time_zones_dropdown_options(void);
size_t time_zones_count(void);
const char *time_zones_name(uint8_t index);
esp_err_t time_zones_apply(uint8_t index);
