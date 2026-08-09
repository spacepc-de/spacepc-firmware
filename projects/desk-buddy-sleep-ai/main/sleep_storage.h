#pragma once

#include "esp_err.h"
#include "sleep_session.h"

esp_err_t sleep_storage_append_event(const snore_event_t *event);
esp_err_t sleep_storage_append_summary(const sleep_session_t *session, const sleep_summary_t *summary);
