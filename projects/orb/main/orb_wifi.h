#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool initialized;
    bool connected;
    bool time_synced;
    char ip_address[16];
} orb_wifi_status_t;

esp_err_t orb_wifi_init(const char *ssid, const char *password);
esp_err_t orb_wifi_connect(const char *ssid, const char *password);
void orb_wifi_get_status(orb_wifi_status_t *status);
