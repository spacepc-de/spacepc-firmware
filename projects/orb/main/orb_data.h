#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "orb_settings.h"

#define ORB_MAX_AIRCRAFT 20
#define ORB_MAX_SATELLITES 18

typedef struct {
    char callsign[12];
    char registration[12];
    char type[8];
    double latitude;
    double longitude;
    float altitude_ft;
    float speed_knots;
    float track_deg;
    float distance_nm;
    float bearing_deg;
    bool on_ground;
} orb_aircraft_t;

typedef struct {
    char name[25];
    uint32_t catalog_id;
    double epoch_unix;
    double mean_motion;
    double eccentricity;
    double inclination;
    double raan;
    double argument_perigee;
    double mean_anomaly;
    double latitude;
    double longitude;
} orb_satellite_t;

typedef struct {
    orb_aircraft_t aircraft[ORB_MAX_AIRCRAFT];
    size_t aircraft_count;
    orb_satellite_t satellites[ORB_MAX_SATELLITES];
    size_t satellite_count;
    double iss_latitude;
    double iss_longitude;
    float iss_altitude_km;
    float iss_velocity_kmh;
    char iss_visibility[12];
    uint64_t aircraft_updated_unix;
    uint64_t iss_updated_unix;
    uint64_t catalog_updated_unix;
    bool aircraft_live;
    bool iss_live;
    bool satellites_live;
    bool refreshing;
    char last_error[48];
} orb_data_snapshot_t;

esp_err_t orb_data_init(const orb_settings_t *settings);
void orb_data_set_observer(double latitude, double longitude, uint16_t radius_nm);
void orb_data_get_snapshot(orb_data_snapshot_t *snapshot);
void orb_data_refresh_now(void);
