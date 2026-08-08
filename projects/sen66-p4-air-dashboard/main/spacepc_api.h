#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sen66.h"

esp_err_t spacepc_api_start(void);
void spacepc_api_update(const sen66_data_t *data, bool available);
