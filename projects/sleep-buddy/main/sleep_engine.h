#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sleep_session.h"

typedef struct {
    bool model_available;
    bool monitoring;
    bool recording_audio;
    bool event_active;
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
esp_err_t sleep_engine_set_event_thresholds(uint8_t start_probability, uint8_t end_probability);
void sleep_engine_get_status(sleep_engine_status_t *status);
bool sleep_engine_get_last_summary(sleep_summary_t *summary);
bool sleep_engine_get_last_times(uint64_t *started_unix_s, uint64_t *ended_unix_s);
bool sleep_engine_get_last_timeline(uint8_t timeline[SLEEP_TIMELINE_BYTES]);
