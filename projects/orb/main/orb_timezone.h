#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool success;
    double latitude;
    double longitude;
    int32_t utc_offset_seconds;
    char timezone[40];
    char message[64];
} orb_timezone_result_t;

esp_err_t orb_timezone_init(void);
bool orb_timezone_request(double latitude, double longitude);
bool orb_timezone_take_result(orb_timezone_result_t *result);
bool orb_timezone_lookup_now(double latitude, double longitude, orb_timezone_result_t *result);
