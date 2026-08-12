#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

esp_err_t orb_net_init(void);
bool orb_net_take(TickType_t timeout);
void orb_net_give(void);
