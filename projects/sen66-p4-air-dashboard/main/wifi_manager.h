#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
bool wifi_manager_connected(void);
