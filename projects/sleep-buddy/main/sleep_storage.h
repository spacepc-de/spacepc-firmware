#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "sleep_session.h"

typedef struct {
    uint64_t started_unix_s;
    uint64_t ended_unix_s;
    sleep_summary_t summary;
    uint8_t snore_timeline[SLEEP_TIMELINE_BYTES];
} sleep_night_record_t;

esp_err_t sleep_storage_append_event(const snore_event_t *event);
esp_err_t sleep_storage_append_summary(const sleep_session_t *session, const sleep_summary_t *summary);
esp_err_t sleep_storage_load_last_summary(sleep_session_t *session, sleep_summary_t *summary);
esp_err_t sleep_storage_load_recent(sleep_night_record_t *records, size_t capacity, size_t *count);
esp_err_t sleep_storage_checkpoint_begin(const sleep_session_t *session,
                                         const sleep_summary_t *summary);
esp_err_t sleep_storage_checkpoint_update(const sleep_session_t *session,
                                          const sleep_summary_t *summary);
esp_err_t sleep_storage_recover_interrupted(void);
esp_err_t sleep_storage_checkpoint_clear(void);
