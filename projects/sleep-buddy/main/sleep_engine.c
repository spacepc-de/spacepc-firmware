#include "sleep_engine.h"

#include <string.h>
#include "app_settings.h"
#include "audio_recorder.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sleep_session.h"
#include "sleep_storage.h"
#include "snore_classifier.h"
#include "snore_event_detector.h"
#include "wifi_manager.h"

#define BLOCK_FRAMES 512
#define WINDOW_SAMPLES 64000
#define AUDIO_QUEUE_BLOCKS 96
// MN04 needs about 1.07 s on the P4 while the display is active. A 1.25 s
// stride leaves deterministic headroom and prevents an overnight audio queue
// backlog without changing the two-second classifier input.
#define STEP_SAMPLES 40000

typedef struct { int16_t samples[BLOCK_FRAMES]; } mono_block_t;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static sleep_engine_status_t s_status;
static sleep_session_t s_session;
static snore_event_detector_t s_detector;
static int16_t *s_window;
static size_t s_filled;
static bool s_recording_owned;
static volatile uint32_t s_dropped_blocks;
static sleep_summary_t s_last_summary;
static sleep_session_t s_last_session;
static bool s_has_last_summary;
static uint64_t s_last_checkpoint_ms;

static void configure_detector(const app_settings_t *settings)
{
    snore_detector_config_t config = {
        .start_threshold = settings->start_probability / 100.0f,
        .end_threshold = settings->end_probability / 100.0f,
        // Test profile: a single positive model window can represent one
        // complete snore at a normal 12-20 breaths/minute cadence.
        .start_frames = 1,
        .end_frames = 4,
        .minimum_event_ms = 1000,
    };
    snore_event_detector_init(&s_detector, &config);
}

static void pcm_callback(const int16_t *stereo, size_t frames, void *context)
{
    (void)context;
    if (frames != BLOCK_FRAMES || !s_status.monitoring) return;
    mono_block_t block;
    // The model and board-domain validation both use Mic 2. Never switch
    // capsules inside an inference window: at room-noise level tiny energy
    // changes otherwise cause block boundaries that look like acoustic events.
    for (size_t i = 0; i < frames; ++i) block.samples[i] = stereo[i * 2 + 1];
    if (xQueueSend(s_queue, &block, 0) != pdTRUE) ++s_dropped_blocks;
}

static void engine_task(void *context)
{
    (void)context;
    mono_block_t block;
    while (true) {
        if (xQueueReceive(s_queue, &block, portMAX_DELAY) != pdTRUE) continue;
        size_t offset = 0;
        while (offset < BLOCK_FRAMES) {
            size_t copy = WINDOW_SAMPLES - s_filled;
            size_t remaining = BLOCK_FRAMES - offset;
            if (copy > remaining) copy = remaining;
            memcpy(s_window + s_filled, block.samples + offset, copy * sizeof(int16_t));
            s_filled += copy;
            offset += copy;
            if (s_filled < WINDOW_SAMPLES) continue;

            int64_t started = esp_timer_get_time();
            float probability = 0;
            esp_err_t err = snore_classifier_predict(s_window, WINDOW_SAMPLES, &probability);
            uint32_t inference_ms = (esp_timer_get_time() - started) / 1000;
            uint64_t now_ms = esp_timer_get_time() / 1000;
            recorder_status_t audio;
            audio_recorder_get_status(&audio);
            if (err == ESP_OK) {
                snore_event_t event = {0};
                bool completed_event = false;
                bool save_checkpoint = false;
                sleep_session_t checkpoint_session = {0};
                sleep_summary_t checkpoint_summary = {0};
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (s_status.monitoring) {
                    snore_detector_result_t result = snore_event_detector_process(
                        &s_detector, now_ms, probability, audio.rms_dbfs_ch2, &event);
                    completed_event = result == SNORE_DETECTOR_EVENT_ENDED;
                    if (completed_event) sleep_session_add_event(&s_session, &event);
                    sleep_summary_t summary;
                    sleep_session_summarize(&s_session, now_ms, &summary);
                    if (completed_event || now_ms - s_last_checkpoint_ms >= 30000U) {
                        checkpoint_session = s_session;
                        checkpoint_session.ended_ms = now_ms;
                        checkpoint_session.ended_unix_s = wifi_manager_unix_seconds();
                        checkpoint_summary = summary;
                        if (!checkpoint_session.started_unix_s && checkpoint_session.ended_unix_s &&
                            checkpoint_summary.monitored_ms) {
                            checkpoint_session.started_unix_s = checkpoint_session.ended_unix_s -
                                checkpoint_summary.monitored_ms / 1000U;
                        }
                        s_last_checkpoint_ms = now_ms;
                        save_checkpoint = true;
                    }
                    s_status.probability = probability;
                    s_status.inference_ms = inference_ms;
                    s_status.event_active = s_detector.active;
                    s_status.event_count = summary.event_count;
                    s_status.monitored_ms = summary.monitored_ms;
                    s_status.snore_ms = summary.snore_ms;
                    s_status.acoustic_score = summary.acoustic_score;
                    strlcpy(s_status.state, s_detector.active ? "Snoring event" :
                            (s_status.recording_audio ? "Listening + recording" : "Listening"),
                            sizeof(s_status.state));
                }
                xSemaphoreGive(s_lock);
                if (completed_event) sleep_storage_append_event(&event);
                if (save_checkpoint) {
                    esp_err_t checkpoint_err = sleep_storage_checkpoint_update(
                        &checkpoint_session, &checkpoint_summary);
                    if (checkpoint_err != ESP_OK) {
                        ESP_LOGW("sleep_engine", "Checkpoint failed: %s",
                                 esp_err_to_name(checkpoint_err));
                    }
                }
            }
            static unsigned inference_count;
            if (++inference_count == 1 || inference_count % 10 == 0) {
                ESP_LOGI("sleep_engine", "Inference %lu ms, queued %u blocks, dropped %lu",
                         (unsigned long)inference_ms,
                         (unsigned)uxQueueMessagesWaiting(s_queue),
                         (unsigned long)s_dropped_blocks);
            }
            memmove(s_window, s_window + STEP_SAMPLES,
                    (WINDOW_SAMPLES - STEP_SAMPLES) * sizeof(int16_t));
            s_filled = WINDOW_SAMPLES - STEP_SAMPLES;
            // TFLite Micro is CPU intensive. Give IDLE0 and the display stack a
            // guaranteed scheduling window after every classification.
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

esp_err_t sleep_engine_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    // Hold 1.536 s of mono audio. MN04 inference peaks around 1.14 s on the
    // board, so capture can continue losslessly while the classifier runs.
    s_queue = xQueueCreate(AUDIO_QUEUE_BLOCKS, sizeof(mono_block_t));
    s_window = heap_caps_malloc(WINDOW_SAMPLES * sizeof(int16_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_window) {
        s_window = heap_caps_malloc(WINDOW_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
    }
    if (!s_lock || !s_queue || !s_window) return ESP_ERR_NO_MEM;
    snore_classifier_info_t info;
    snore_classifier_get_info(&info);
    s_status.model_available = info.model_available;
    strlcpy(s_status.state, info.model_available ? "Ready" : "Model not installed", sizeof(s_status.state));
    app_settings_t settings;
    app_settings_load(&settings);
    configure_detector(&settings);
    esp_err_t recovery_err = sleep_storage_recover_interrupted();
    if (recovery_err != ESP_OK && recovery_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW("sleep_engine", "Interrupted-night recovery failed: %s",
                 esp_err_to_name(recovery_err));
    }
    s_has_last_summary = sleep_storage_load_last_summary(&s_last_session, &s_last_summary) == ESP_OK;
    audio_recorder_set_pcm_callback(pcm_callback, NULL);
    // UI/DSI maintenance must be able to preempt inference on CPU0.
    return xTaskCreatePinnedToCore(engine_task, "snore_inference", 8192, NULL, 2, NULL, 0) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t set_monitoring(bool enabled, bool record_audio)
{
    if (enabled && !s_status.model_available) return ESP_ERR_NOT_FOUND;
    app_settings_t settings;
    app_settings_load(&settings);
    recorder_status_t recorder;
    audio_recorder_get_status(&recorder);
    if (enabled && !recorder.ready) return ESP_ERR_INVALID_STATE;
    if (enabled && !recorder.sd_mounted) return ESP_ERR_NOT_FOUND;

    if (enabled && !s_status.monitoring && record_audio && !recorder.recording) {
        esp_err_t record_err = audio_recorder_start();
        if (record_err != ESP_OK) {
            ESP_LOGE("sleep_engine", "Night recording failed: %s", esp_err_to_name(record_err));
            return record_err;
        }
        s_recording_owned = true;
    } else if (enabled && !s_status.monitoring) {
        // A manual recording must never be stopped automatically with the night.
        s_recording_owned = false;
    }

    bool stop_owned_recording = false;
    bool completed_session = false;
    bool completed_final_event = false;
    bool started_session = false;
    esp_err_t record_result = ESP_OK;
    esp_err_t summary_result = ESP_OK;
    snore_event_t final_event = {0};
    sleep_session_t finished_session = {0};
    sleep_summary_t finished_summary = {0};
    sleep_session_t initial_checkpoint_session = {0};
    sleep_summary_t initial_checkpoint_summary = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (enabled && !s_status.monitoring) {
        memset(&s_session, 0, sizeof(s_session));
        sleep_session_start(&s_session, esp_timer_get_time() / 1000,
                            wifi_manager_unix_seconds());
        initial_checkpoint_session = s_session;
        sleep_session_summarize(&s_session, s_session.started_ms, &initial_checkpoint_summary);
        s_last_checkpoint_ms = s_session.started_ms;
        started_session = true;
        configure_detector(&settings);
        s_filled = 0;
        s_dropped_blocks = 0;
        xQueueReset(s_queue);
        s_status.probability = 0;
        s_status.event_count = 0;
        s_status.monitored_ms = 0;
        s_status.snore_ms = 0;
        s_status.acoustic_score = 100;
        s_status.recording_audio = recorder.recording || s_recording_owned;
        strlcpy(s_status.state, s_status.recording_audio ? "Listening + recording" : "Preparing audio",
                sizeof(s_status.state));
    } else if (!enabled && s_status.monitoring) {
        uint64_t ended_ms = esp_timer_get_time() / 1000;
        completed_final_event = snore_event_detector_finish(&s_detector, ended_ms, &final_event)
                                == SNORE_DETECTOR_EVENT_ENDED;
        if (completed_final_event) sleep_session_add_event(&s_session, &final_event);
        sleep_session_finish(&s_session, ended_ms, wifi_manager_unix_seconds(), &finished_summary);
        finished_session = s_session;
        s_last_session = finished_session;
        s_last_summary = finished_summary;
        s_has_last_summary = true;
        s_status.event_active = false;
        s_status.event_count = finished_summary.event_count;
        s_status.monitored_ms = finished_summary.monitored_ms;
        s_status.snore_ms = finished_summary.snore_ms;
        s_status.acoustic_score = finished_summary.acoustic_score;
        completed_session = true;
        stop_owned_recording = s_recording_owned;
        s_recording_owned = false;
        s_status.recording_audio = false;
        strlcpy(s_status.state, stop_owned_recording ? "Saving night recording" : "Session saved",
                sizeof(s_status.state));
    }
    s_status.monitoring = enabled;
    xSemaphoreGive(s_lock);

    if (started_session) {
        esp_err_t checkpoint_err = sleep_storage_checkpoint_begin(
            &initial_checkpoint_session, &initial_checkpoint_summary);
        if (checkpoint_err != ESP_OK) {
            ESP_LOGW("sleep_engine", "Initial checkpoint failed: %s",
                     esp_err_to_name(checkpoint_err));
        }
    }
    if (stop_owned_recording) {
        recorder_status_t current_recorder = {0};
        audio_recorder_get_status(&current_recorder);
        record_result = current_recorder.recording ? audio_recorder_stop() : ESP_OK;
    }
    if (completed_final_event) {
        esp_err_t event_result = sleep_storage_append_event(&final_event);
        if (event_result != ESP_OK) {
            ESP_LOGW("sleep_engine", "Final event save failed: %s", esp_err_to_name(event_result));
        }
    }
    if (completed_session) {
        summary_result = sleep_storage_append_summary(&finished_session, &finished_summary);
        if (summary_result != ESP_OK) {
            ESP_LOGE("sleep_engine", "Night summary save failed: %s", esp_err_to_name(summary_result));
        } else {
            ESP_LOGI("sleep_engine",
                     "Night saved to microSD: score %u, monitored %llu min, snoring %llu s, events %lu",
                     finished_summary.acoustic_score,
                     (unsigned long long)(finished_summary.monitored_ms / 60000),
                     (unsigned long long)(finished_summary.snore_ms / 1000),
                     (unsigned long)finished_summary.event_count);
            esp_err_t clear_err = sleep_storage_checkpoint_clear();
            if (clear_err != ESP_OK) {
                ESP_LOGW("sleep_engine", "Checkpoint cleanup failed: %s", esp_err_to_name(clear_err));
            }
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (summary_result != ESP_OK) {
            strlcpy(s_status.state, "Summary save failed", sizeof(s_status.state));
        } else if (record_result != ESP_OK) {
            strlcpy(s_status.state, "Summary saved; WAV error", sizeof(s_status.state));
        } else {
            strlcpy(s_status.state, stop_owned_recording ? "Session + WAV saved" : "Session saved",
                    sizeof(s_status.state));
        }
        xSemaphoreGive(s_lock);
    }
    if (summary_result != ESP_OK) return summary_result;
    return record_result;
}

esp_err_t sleep_engine_start_monitoring(bool record_audio)
{
    return set_monitoring(true, record_audio);
}

esp_err_t sleep_engine_set_monitoring(bool enabled)
{
    if (!enabled) return set_monitoring(false, false);
    app_settings_t settings;
    app_settings_load(&settings);
    return set_monitoring(true, settings.record_during_monitoring);
}

esp_err_t sleep_engine_set_event_thresholds(uint8_t start_probability, uint8_t end_probability)
{
    if (!s_lock || start_probability < 5 || start_probability > 90 ||
        end_probability < 5 || end_probability > start_probability) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_detector.config.start_threshold = start_probability / 100.0f;
    s_detector.config.end_threshold = end_probability / 100.0f;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void sleep_engine_get_status(sleep_engine_status_t *status)
{
    if (!s_lock) { memset(status, 0, sizeof(*status)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    if (s_status.monitoring) {
        sleep_summary_t live_summary;
        sleep_session_summarize(&s_session, esp_timer_get_time() / 1000, &live_summary);
        status->monitored_ms = live_summary.monitored_ms;
    }
    xSemaphoreGive(s_lock);
}

bool sleep_engine_get_last_summary(sleep_summary_t *summary)
{
    if (!summary || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool available = s_has_last_summary;
    if (available) *summary = s_last_summary;
    xSemaphoreGive(s_lock);
    return available;
}

bool sleep_engine_get_last_times(uint64_t *started_unix_s, uint64_t *ended_unix_s)
{
    if (!started_unix_s || !ended_unix_s || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool available = s_has_last_summary;
    if (available) {
        *started_unix_s = s_last_session.started_unix_s;
        *ended_unix_s = s_last_session.ended_unix_s;
    }
    xSemaphoreGive(s_lock);
    return available;
}

bool sleep_engine_get_last_timeline(uint8_t timeline[SLEEP_TIMELINE_BYTES])
{
    if (!timeline || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool available = s_has_last_summary;
    if (available) memcpy(timeline, s_last_session.snore_timeline, SLEEP_TIMELINE_BYTES);
    xSemaphoreGive(s_lock);
    return available;
}
