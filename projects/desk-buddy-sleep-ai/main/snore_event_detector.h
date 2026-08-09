#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float start_threshold;
    float end_threshold;
    uint8_t start_frames;
    uint8_t end_frames;
    uint32_t minimum_event_ms;
} snore_detector_config_t;

typedef struct {
    uint64_t started_ms;
    uint64_t ended_ms;
    float peak_probability;
    float mean_probability;
    float peak_dbfs;
} snore_event_t;

typedef enum {
    SNORE_DETECTOR_NONE,
    SNORE_DETECTOR_EVENT_STARTED,
    SNORE_DETECTOR_EVENT_ENDED,
} snore_detector_result_t;

typedef struct {
    snore_detector_config_t config;
    bool active;
    uint8_t above_count;
    uint8_t below_count;
    uint32_t probability_samples;
    float probability_sum;
    snore_event_t current;
} snore_event_detector_t;

void snore_event_detector_init(snore_event_detector_t *detector, const snore_detector_config_t *config);
snore_detector_result_t snore_event_detector_process(snore_event_detector_t *detector,
                                                      uint64_t timestamp_ms, float probability,
                                                      float level_dbfs, snore_event_t *completed);
