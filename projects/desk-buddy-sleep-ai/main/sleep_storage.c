#include "sleep_storage.h"

#include <stdio.h>
#include "bsp/esp-bsp.h"

esp_err_t sleep_storage_append_event(const snore_event_t *event)
{
    FILE *file = fopen(BSP_SD_MOUNT_POINT "/EVENTS.JL", "a");
    if (!file) return ESP_FAIL;
    int written = fprintf(file,
        "{\"start_ms\":%llu,\"end_ms\":%llu,\"confidence\":%.4f,\"peak_dbfs\":%.2f}\n",
        (unsigned long long)event->started_ms, (unsigned long long)event->ended_ms,
        event->mean_probability, event->peak_dbfs);
    bool ok = written > 0 && fflush(file) == 0 && fclose(file) == 0;
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t sleep_storage_append_summary(const sleep_session_t *session, const sleep_summary_t *summary)
{
    FILE *file = fopen(BSP_SD_MOUNT_POINT "/NIGHTS.JL", "a");
    if (!file) return ESP_FAIL;
    int written = fprintf(file,
        "{\"start_ms\":%llu,\"end_ms\":%llu,\"monitored_ms\":%llu,"
        "\"snore_ms\":%llu,\"events\":%lu,\"longest_ms\":%llu,"
        "\"snore_percent\":%.3f,\"mean_confidence\":%.4f,\"peak_dbfs\":%.2f,"
        "\"acoustic_score\":%u}\n",
        (unsigned long long)session->started_ms, (unsigned long long)session->ended_ms,
        (unsigned long long)summary->monitored_ms, (unsigned long long)summary->snore_ms,
        (unsigned long)summary->event_count, (unsigned long long)summary->longest_event_ms,
        summary->snore_percent, summary->mean_confidence, summary->loudest_dbfs,
        summary->acoustic_score);
    bool ok = written > 0 && fflush(file) == 0 && fclose(file) == 0;
    return ok ? ESP_OK : ESP_FAIL;
}
