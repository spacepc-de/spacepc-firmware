#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t start_hour;
    uint8_t start_minute;
    uint8_t end_hour;
    uint8_t end_minute;
    uint8_t night_brightness;
    uint8_t microphone_channel;
    // Retained in-place so existing version-7 NVS blobs, including WiFi
    // credentials, remain binary compatible. Audio gain is fixed in firmware.
    uint8_t reserved_microphone_gain_db;
    uint8_t start_probability;
    uint8_t end_probability;
    // Former sensitivity selector; retained only for NVS layout compatibility.
    uint8_t reserved_model_profile;
    bool schedule_enabled;
    bool display_off_during_monitoring;
    bool retain_event_audio;
    bool record_during_monitoring;
    uint8_t timezone_index;
    char wifi_ssid[33];
    char wifi_password[65];
} app_settings_t;

void app_settings_defaults(app_settings_t *settings);
esp_err_t app_settings_load(app_settings_t *settings);
esp_err_t app_settings_save(const app_settings_t *settings);
