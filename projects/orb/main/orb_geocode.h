#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool success;
    double latitude;
    double longitude;
    int32_t utc_offset_seconds;
    char city[64];
    char timezone[40];
    char message[80];
} orb_geocode_result_t;

esp_err_t orb_geocode_init(void);
bool orb_geocode_request(const char *city);
bool orb_geocode_take_result(orb_geocode_result_t *result);
