#include "sleep_session.h"

#include <string.h>

void sleep_session_start(sleep_session_t *session, uint64_t timestamp_ms)
{
    memset(session, 0, sizeof(*session));
    session->active = true;
    session->started_ms = timestamp_ms;
    session->loudest_dbfs = -96.0f;
}

void sleep_session_add_event(sleep_session_t *session, const snore_event_t *event)
{
    if (!session->active || event->ended_ms <= event->started_ms) return;
    uint64_t duration = event->ended_ms - event->started_ms;
    session->snore_ms += duration;
    if (duration > session->longest_event_ms) session->longest_event_ms = duration;
    if (event->peak_dbfs > session->loudest_dbfs) session->loudest_dbfs = event->peak_dbfs;
    session->confidence_sum += event->mean_probability;
    session->event_count++;
}

void sleep_session_summarize(const sleep_session_t *session, uint64_t now_ms, sleep_summary_t *summary)
{
    memset(summary, 0, sizeof(*summary));
    uint64_t end = session->ended_ms ? session->ended_ms : now_ms;
    summary->monitored_ms = end > session->started_ms ? end - session->started_ms : 0;
    summary->snore_ms = session->snore_ms;
    summary->longest_event_ms = session->longest_event_ms;
    summary->event_count = session->event_count;
    summary->snore_percent = summary->monitored_ms
        ? 100.0f * summary->snore_ms / summary->monitored_ms : 0;
    summary->mean_confidence = session->event_count
        ? session->confidence_sum / session->event_count : 0;
    summary->loudest_dbfs = session->loudest_dbfs;

    float hours = summary->monitored_ms / 3600000.0f;
    float events_per_hour = hours > 0 ? summary->event_count / hours : 0;
    float penalty = summary->snore_percent * 1.8f;
    penalty += events_per_hour > 20 ? 20 : events_per_hour;
    penalty += summary->longest_event_ms > 600000 ? 15 : summary->longest_event_ms / 40000.0f;
    if (penalty > 80) penalty = 80;
    summary->acoustic_score = (uint8_t)(100.0f - penalty + 0.5f);
}

void sleep_session_finish(sleep_session_t *session, uint64_t timestamp_ms, sleep_summary_t *summary)
{
    session->ended_ms = timestamp_ms;
    session->active = false;
    sleep_session_summarize(session, timestamp_ms, summary);
}
