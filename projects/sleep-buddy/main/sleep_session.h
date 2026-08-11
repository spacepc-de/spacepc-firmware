#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "snore_event_detector.h"

#define SLEEP_TIMELINE_MINUTES (24U * 60U)
#define SLEEP_TIMELINE_BYTES ((SLEEP_TIMELINE_MINUTES + 7U) / 8U)

typedef struct {
    bool active;
    uint64_t started_ms;
    uint64_t ended_ms;
    uint64_t started_unix_s;
    uint64_t ended_unix_s;
    uint64_t snore_ms;
    uint64_t longest_event_ms;
    uint32_t event_count;
    float confidence_sum;
    float loudest_dbfs;
    float strongest_probability;
    uint8_t snore_timeline[SLEEP_TIMELINE_BYTES];
} sleep_session_t;

typedef struct {
    uint64_t monitored_ms;
    uint64_t snore_ms;
    uint64_t longest_event_ms;
    uint32_t event_count;
    float snore_percent;
    float mean_confidence;
    float loudest_dbfs;
    float strongest_probability;
    uint8_t acoustic_score;
} sleep_summary_t;

void sleep_session_start(sleep_session_t *session, uint64_t timestamp_ms, uint64_t unix_seconds);
void sleep_session_add_event(sleep_session_t *session, const snore_event_t *event);
void sleep_session_finish(sleep_session_t *session, uint64_t timestamp_ms, uint64_t unix_seconds,
                          sleep_summary_t *summary);
void sleep_session_summarize(const sleep_session_t *session, uint64_t now_ms, sleep_summary_t *summary);
bool sleep_session_timeline_minute(const uint8_t timeline[SLEEP_TIMELINE_BYTES], uint16_t minute);
