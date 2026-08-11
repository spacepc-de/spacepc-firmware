#include "snore_event_detector.h"

#include <string.h>

static snore_detector_result_t finish_event(snore_event_detector_t *detector,
                                             uint64_t timestamp_ms,
                                             snore_event_t *completed)
{
    detector->current.ended_ms = detector->candidate_ended_ms
                                   ? detector->candidate_ended_ms : timestamp_ms;
    detector->current.mean_probability = detector->probability_samples
        ? detector->probability_sum / detector->probability_samples : 0;
    detector->active = false;
    detector->above_count = 0;
    detector->below_count = 0;
    detector->candidate_started_ms = 0;
    detector->candidate_ended_ms = 0;
    if (detector->current.ended_ms <= detector->current.started_ms ||
        detector->current.ended_ms - detector->current.started_ms < detector->config.minimum_event_ms) {
        return SNORE_DETECTOR_NONE;
    }
    if (completed) *completed = detector->current;
    return SNORE_DETECTOR_EVENT_ENDED;
}

void snore_event_detector_init(snore_event_detector_t *detector, const snore_detector_config_t *config)
{
    memset(detector, 0, sizeof(*detector));
    detector->config = *config;
}

snore_detector_result_t snore_event_detector_process(snore_event_detector_t *detector,
                                                      uint64_t timestamp_ms, float probability,
                                                      float level_dbfs, snore_event_t *completed)
{
    if (probability < 0) probability = 0;
    if (probability > 1) probability = 1;

    if (!detector->active) {
        if (probability >= detector->config.start_threshold) {
            if (detector->above_count == 0) detector->candidate_started_ms = timestamp_ms;
            detector->above_count++;
        } else {
            detector->above_count = 0;
            detector->candidate_started_ms = 0;
        }
        if (detector->above_count < detector->config.start_frames) return SNORE_DETECTOR_NONE;
        detector->active = true;
        detector->below_count = 0;
        detector->candidate_ended_ms = 0;
        detector->probability_samples = 1;
        detector->probability_sum = probability;
        detector->current = (snore_event_t) {
            .started_ms = detector->candidate_started_ms,
            .peak_probability = probability,
            .peak_dbfs = level_dbfs,
        };
        return SNORE_DETECTOR_EVENT_STARTED;
    }

    detector->probability_samples++;
    detector->probability_sum += probability;
    if (probability > detector->current.peak_probability) detector->current.peak_probability = probability;
    if (level_dbfs > detector->current.peak_dbfs) detector->current.peak_dbfs = level_dbfs;
    if (probability <= detector->config.end_threshold) {
        if (detector->below_count == 0) detector->candidate_ended_ms = timestamp_ms;
        detector->below_count++;
    } else {
        detector->below_count = 0;
        detector->candidate_ended_ms = 0;
    }
    if (detector->below_count < detector->config.end_frames) return SNORE_DETECTOR_NONE;
    return finish_event(detector, timestamp_ms, completed);
}

snore_detector_result_t snore_event_detector_finish(snore_event_detector_t *detector,
                                                     uint64_t timestamp_ms,
                                                     snore_event_t *completed)
{
    if (!detector->active) return SNORE_DETECTOR_NONE;
    return finish_event(detector, timestamp_ms, completed);
}
