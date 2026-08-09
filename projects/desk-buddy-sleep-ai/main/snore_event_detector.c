#include "snore_event_detector.h"

#include <string.h>

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
        detector->above_count = probability >= detector->config.start_threshold
                                    ? detector->above_count + 1 : 0;
        if (detector->above_count < detector->config.start_frames) return SNORE_DETECTOR_NONE;
        detector->active = true;
        detector->below_count = 0;
        detector->probability_samples = 1;
        detector->probability_sum = probability;
        detector->current = (snore_event_t) {
            .started_ms = timestamp_ms,
            .peak_probability = probability,
            .peak_dbfs = level_dbfs,
        };
        return SNORE_DETECTOR_EVENT_STARTED;
    }

    detector->probability_samples++;
    detector->probability_sum += probability;
    if (probability > detector->current.peak_probability) detector->current.peak_probability = probability;
    if (level_dbfs > detector->current.peak_dbfs) detector->current.peak_dbfs = level_dbfs;
    detector->below_count = probability <= detector->config.end_threshold
                                ? detector->below_count + 1 : 0;
    if (detector->below_count < detector->config.end_frames) return SNORE_DETECTOR_NONE;

    detector->current.ended_ms = timestamp_ms;
    detector->current.mean_probability = detector->probability_sum / detector->probability_samples;
    detector->active = false;
    detector->above_count = 0;
    detector->below_count = 0;
    if (detector->current.ended_ms - detector->current.started_ms < detector->config.minimum_event_ms) {
        return SNORE_DETECTOR_NONE;
    }
    if (completed) *completed = detector->current;
    return SNORE_DETECTOR_EVENT_ENDED;
}
