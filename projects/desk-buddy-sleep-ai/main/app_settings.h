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
    uint8_t microphone_gain_db;
    uint8_t start_probability;
    uint8_t end_probability;
    bool schedule_enabled;
    bool display_off_during_monitoring;
    bool retain_event_audio;
} app_settings_t;

void app_settings_defaults(app_settings_t *settings);
esp_err_t app_settings_load(app_settings_t *settings);
esp_err_t app_settings_save(const app_settings_t *settings);
