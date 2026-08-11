#include "sleep_session.h"

#include <string.h>

static void mark_timeline_minute(uint8_t timeline[SLEEP_TIMELINE_BYTES], uint16_t minute)
{
    if (minute >= SLEEP_TIMELINE_MINUTES) return;
    timeline[minute / 8U] |= (uint8_t)(1U << (minute % 8U));
}

bool sleep_session_timeline_minute(const uint8_t timeline[SLEEP_TIMELINE_BYTES], uint16_t minute)
{
    if (!timeline || minute >= SLEEP_TIMELINE_MINUTES) return false;
    return (timeline[minute / 8U] & (uint8_t)(1U << (minute % 8U))) != 0;
}

void sleep_session_start(sleep_session_t *session, uint64_t timestamp_ms, uint64_t unix_seconds)
{
    memset(session, 0, sizeof(*session));
    session->active = true;
    session->started_ms = timestamp_ms;
    session->started_unix_s = unix_seconds;
    session->loudest_dbfs = -96.0f;
}

void sleep_session_add_event(sleep_session_t *session, const snore_event_t *event)
{
    if (!session->active || event->ended_ms <= event->started_ms) return;
    uint64_t duration = event->ended_ms - event->started_ms;
    session->snore_ms += duration;
    if (duration > session->longest_event_ms) session->longest_event_ms = duration;
    if (event->peak_dbfs > session->loudest_dbfs) session->loudest_dbfs = event->peak_dbfs;
    if (event->peak_probability > session->strongest_probability) {
        session->strongest_probability = event->peak_probability;
    }
    uint64_t relative_start = event->started_ms > session->started_ms
        ? event->started_ms - session->started_ms : 0;
    uint64_t relative_end = event->ended_ms > session->started_ms
        ? event->ended_ms - session->started_ms : 0;
    uint64_t first_minute = relative_start / 60000U;
    uint64_t last_minute = relative_end ? (relative_end - 1U) / 60000U : first_minute;
    if (last_minute >= SLEEP_TIMELINE_MINUTES) last_minute = SLEEP_TIMELINE_MINUTES - 1U;
    for (uint64_t minute = first_minute;
         minute <= last_minute && minute < SLEEP_TIMELINE_MINUTES; ++minute) {
        mark_timeline_minute(session->snore_timeline, (uint16_t)minute);
    }
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
    summary->strongest_probability = session->strongest_probability;

    float hours = summary->monitored_ms / 3600000.0f;
    float events_per_hour = hours > 0 ? summary->event_count / hours : 0;
    float penalty = summary->snore_percent * 1.8f;
    penalty += events_per_hour > 20 ? 20 : events_per_hour;
    penalty += summary->longest_event_ms > 600000 ? 15 : summary->longest_event_ms / 40000.0f;
    if (penalty > 80) penalty = 80;
    summary->acoustic_score = (uint8_t)(100.0f - penalty + 0.5f);
}

void sleep_session_finish(sleep_session_t *session, uint64_t timestamp_ms, uint64_t unix_seconds,
                          sleep_summary_t *summary)
{
    session->ended_ms = timestamp_ms;
    session->ended_unix_s = unix_seconds;
    session->active = false;
    sleep_session_summarize(session, timestamp_ms, summary);
    if (!session->started_unix_s && session->ended_unix_s && summary->monitored_ms) {
        session->started_unix_s = session->ended_unix_s - summary->monitored_ms / 1000;
    }
}
