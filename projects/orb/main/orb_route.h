#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool success;
    char callsign[12];
    char origin_code[5];
    char origin_city[32];
    char destination_code[5];
    char destination_city[32];
    uint16_t estimated_minutes;
    char message[48];
} orb_route_result_t;

esp_err_t orb_route_init(void);
bool orb_route_request(const char *callsign, double latitude, double longitude);
bool orb_route_take_result(orb_route_result_t *result);
