#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    ORB_DISTANCE_NAUTICAL_MILES,
    ORB_DISTANCE_KILOMETRES,
    ORB_DISTANCE_MILES,
} orb_distance_unit_t;

typedef enum {
    ORB_ALTITUDE_FEET,
    ORB_ALTITUDE_METRES,
} orb_altitude_unit_t;

typedef enum {
    ORB_SPEED_KNOTS,
    ORB_SPEED_KMH,
    ORB_SPEED_MPH,
} orb_speed_unit_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char city[64];
    double latitude;
    double longitude;
    uint16_t aircraft_radius_nm;
    uint8_t map_zoom;
    uint8_t distance_unit;
    uint8_t altitude_unit;
    uint8_t speed_unit;
    char timezone[40];
    int32_t utc_offset_seconds;
} orb_settings_t;

void orb_settings_defaults(orb_settings_t *settings);
esp_err_t orb_settings_load(orb_settings_t *settings);
esp_err_t orb_settings_save(const orb_settings_t *settings);
