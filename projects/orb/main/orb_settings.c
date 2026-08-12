#include "orb_settings.h"

#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define ORB_SETTINGS_VERSION 4

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    double latitude;
    double longitude;
    uint16_t aircraft_radius_nm;
} orb_settings_v1_t;

typedef struct {
    uint16_t version;
    orb_settings_v1_t values;
} orb_settings_blob_v1_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    double latitude;
    double longitude;
    uint16_t aircraft_radius_nm;
    uint8_t map_zoom;
} orb_settings_v2_t;

typedef struct {
    uint16_t version;
    orb_settings_v2_t values;
} orb_settings_blob_v2_t;

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
} orb_settings_v3_t;

typedef struct {
    uint16_t version;
    orb_settings_v3_t values;
} orb_settings_blob_v3_t;

typedef struct {
    uint16_t version;
    orb_settings_t values;
} orb_settings_blob_t;

void orb_settings_defaults(orb_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    /* Neutral European demo position; users set their observer in Settings. */
    settings->latitude = 48.1372;
    settings->longitude = 11.5756;
    settings->aircraft_radius_nm = 80;
    settings->map_zoom = 8;
    settings->distance_unit = ORB_DISTANCE_NAUTICAL_MILES;
    settings->altitude_unit = ORB_ALTITUDE_FEET;
    settings->speed_unit = ORB_SPEED_KNOTS;
    strlcpy(settings->timezone, "UTC", sizeof(settings->timezone));
}

esp_err_t orb_settings_load(orb_settings_t *settings)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    nvs_handle_t handle;
    err = nvs_open("orb", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        orb_settings_defaults(settings);
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    union {
        orb_settings_blob_t current;
        orb_settings_blob_v3_t v3;
        orb_settings_blob_v2_t v2;
        orb_settings_blob_v1_t legacy;
    } blob;
    size_t size = sizeof(blob);
    err = nvs_get_blob(handle, "settings", &blob, &size);
    nvs_close(handle);
    if (err == ESP_OK && size == sizeof(blob.current) && blob.current.version == ORB_SETTINGS_VERSION) {
        *settings = blob.current.values;
        if (settings->map_zoom < 4 || settings->map_zoom > 14) settings->map_zoom = 8;
        if (settings->distance_unit > ORB_DISTANCE_MILES) settings->distance_unit = ORB_DISTANCE_NAUTICAL_MILES;
        if (settings->altitude_unit > ORB_ALTITUDE_METRES) settings->altitude_unit = ORB_ALTITUDE_FEET;
        if (settings->speed_unit > ORB_SPEED_MPH) settings->speed_unit = ORB_SPEED_KNOTS;
        if (!settings->timezone[0]) strlcpy(settings->timezone, "UTC", sizeof(settings->timezone));
        if (settings->utc_offset_seconds < -43200 || settings->utc_offset_seconds > 50400) {
            settings->utc_offset_seconds = 0;
        }
        return ESP_OK;
    }
    if (err == ESP_OK && size == sizeof(blob.v3) && blob.v3.version == 3) {
        orb_settings_defaults(settings);
        strlcpy(settings->wifi_ssid, blob.v3.values.wifi_ssid, sizeof(settings->wifi_ssid));
        strlcpy(settings->wifi_password, blob.v3.values.wifi_password, sizeof(settings->wifi_password));
        strlcpy(settings->city, blob.v3.values.city, sizeof(settings->city));
        settings->latitude = blob.v3.values.latitude;
        settings->longitude = blob.v3.values.longitude;
        settings->aircraft_radius_nm = blob.v3.values.aircraft_radius_nm;
        settings->map_zoom = blob.v3.values.map_zoom;
        settings->distance_unit = blob.v3.values.distance_unit;
        settings->altitude_unit = blob.v3.values.altitude_unit;
        settings->speed_unit = blob.v3.values.speed_unit;
        return ESP_OK;
    }
    if (err == ESP_OK && size == sizeof(blob.v2) && blob.v2.version == 2) {
        orb_settings_defaults(settings);
        strlcpy(settings->wifi_ssid, blob.v2.values.wifi_ssid, sizeof(settings->wifi_ssid));
        strlcpy(settings->wifi_password, blob.v2.values.wifi_password, sizeof(settings->wifi_password));
        settings->latitude = blob.v2.values.latitude;
        settings->longitude = blob.v2.values.longitude;
        settings->aircraft_radius_nm = blob.v2.values.aircraft_radius_nm;
        settings->map_zoom = blob.v2.values.map_zoom;
        return ESP_OK;
    }
    if (err == ESP_OK && size == sizeof(blob.legacy) && blob.legacy.version == 1) {
        orb_settings_defaults(settings);
        strlcpy(settings->wifi_ssid, blob.legacy.values.wifi_ssid, sizeof(settings->wifi_ssid));
        strlcpy(settings->wifi_password, blob.legacy.values.wifi_password, sizeof(settings->wifi_password));
        settings->latitude = blob.legacy.values.latitude;
        settings->longitude = blob.legacy.values.longitude;
        settings->aircraft_radius_nm = blob.legacy.values.aircraft_radius_nm;
        return ESP_OK;
    }
    {
        orb_settings_defaults(settings);
        return ESP_OK;
    }
}

esp_err_t orb_settings_save(const orb_settings_t *settings)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("orb", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    const orb_settings_blob_t blob = {.version = ORB_SETTINGS_VERSION, .values = *settings};
    err = nvs_set_blob(handle, "settings", &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
