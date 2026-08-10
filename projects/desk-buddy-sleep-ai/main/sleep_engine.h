#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool model_available;
    bool monitoring;
    bool recording_audio;
    bool event_active;
    uint8_t model_profile;
    float probability;
    uint32_t inference_ms;
    uint32_t event_count;
    uint64_t monitored_ms;
    uint64_t snore_ms;
    uint8_t acoustic_score;
    char state[40];
} sleep_engine_status_t;

esp_err_t sleep_engine_init(void);
esp_err_t sleep_engine_start_monitoring(bool record_audio);
esp_err_t sleep_engine_set_monitoring(bool enabled);
void sleep_engine_get_status(sleep_engine_status_t *status);
