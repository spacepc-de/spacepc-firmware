#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool initialized;
    bool connected;
    bool time_synced;
    char ip_address[16];
} wifi_manager_status_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
void wifi_manager_get_status(wifi_manager_status_t *status);
bool wifi_manager_time_valid(void);
uint64_t wifi_manager_unix_seconds(void);
