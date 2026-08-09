#include "sleep_engine.h"

#include <string.h>
#include "app_settings.h"
#include "audio_recorder.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sleep_session.h"
#include "sleep_storage.h"
#include "snore_classifier.h"
#include "snore_event_detector.h"

#define BLOCK_FRAMES 512
#define WINDOW_SAMPLES 16000
#define STEP_SAMPLES 8000

typedef struct { int16_t samples[BLOCK_FRAMES]; } mono_block_t;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static sleep_engine_status_t s_status;
static sleep_session_t s_session;
static snore_event_detector_t s_detector;
static int16_t s_window[WINDOW_SAMPLES];
static size_t s_filled;

static void pcm_callback(const int16_t *stereo, size_t frames, void *context)
{
    (void)context;
    if (frames != BLOCK_FRAMES || !s_status.monitoring) return;
    mono_block_t block;
    for (size_t i = 0; i < frames; ++i) block.samples[i] = stereo[i * 2 + 1];
    xQueueSend(s_queue, &block, 0);
}

static void engine_task(void *context)
{
    (void)context;
    mono_block_t block;
    while (true) {
        if (xQueueReceive(s_queue, &block, portMAX_DELAY) != pdTRUE) continue;
        size_t copy = WINDOW_SAMPLES - s_filled;
        if (copy > BLOCK_FRAMES) copy = BLOCK_FRAMES;
        memcpy(s_window + s_filled, block.samples, copy * sizeof(int16_t));
        s_filled += copy;
        if (s_filled < WINDOW_SAMPLES) continue;

        int64_t started = esp_timer_get_time();
        float probability = 0;
        esp_err_t err = snore_classifier_predict(s_window, WINDOW_SAMPLES, &probability);
        uint32_t inference_ms = (esp_timer_get_time() - started) / 1000;
        uint64_t now_ms = esp_timer_get_time() / 1000;
        recorder_status_t audio;
        audio_recorder_get_status(&audio);
        if (err == ESP_OK) {
            snore_event_t event;
            snore_detector_result_t result = snore_event_detector_process(
                &s_detector, now_ms, probability, audio.rms_dbfs_ch2, &event);
            if (result == SNORE_DETECTOR_EVENT_ENDED) {
                sleep_session_add_event(&s_session, &event);
                sleep_storage_append_event(&event);
            }
            sleep_summary_t summary;
            sleep_session_summarize(&s_session, now_ms, &summary);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.probability = probability;
            s_status.inference_ms = inference_ms;
            s_status.event_active = s_detector.active;
            s_status.event_count = summary.event_count;
            s_status.monitored_ms = summary.monitored_ms;
            s_status.snore_ms = summary.snore_ms;
            s_status.acoustic_score = summary.acoustic_score;
            strlcpy(s_status.state, s_detector.active ? "Snoring event" : "Listening", sizeof(s_status.state));
            xSemaphoreGive(s_lock);
        }
        memmove(s_window, s_window + STEP_SAMPLES, (WINDOW_SAMPLES - STEP_SAMPLES) * sizeof(int16_t));
        s_filled = WINDOW_SAMPLES - STEP_SAMPLES;
    }
}

esp_err_t sleep_engine_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(4, sizeof(mono_block_t));
    if (!s_lock || !s_queue) return ESP_ERR_NO_MEM;
    snore_classifier_info_t info;
    snore_classifier_get_info(&info);
    s_status.model_available = info.model_available;
    strlcpy(s_status.state, info.model_available ? "Ready" : "Model not installed", sizeof(s_status.state));
    app_settings_t settings;
    app_settings_load(&settings);
    snore_detector_config_t config = {
        .start_threshold = settings.start_probability / 100.0f,
        .end_threshold = settings.end_probability / 100.0f,
        .start_frames = 3, .end_frames = 4, .minimum_event_ms = 1500,
    };
    snore_event_detector_init(&s_detector, &config);
    audio_recorder_set_pcm_callback(pcm_callback, NULL);
    return xTaskCreatePinnedToCore(engine_task, "snore_inference", 8192, NULL, 8, NULL, 0) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t sleep_engine_set_monitoring(bool enabled)
{
    if (enabled && !s_status.model_available) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (enabled && !s_status.monitoring) {
        memset(&s_session, 0, sizeof(s_session));
        sleep_session_start(&s_session, esp_timer_get_time() / 1000);
        s_filled = 0;
        xQueueReset(s_queue);
        strlcpy(s_status.state, "Preparing audio", sizeof(s_status.state));
    } else if (!enabled && s_status.monitoring) {
        sleep_summary_t summary;
        sleep_session_finish(&s_session, esp_timer_get_time() / 1000, &summary);
        sleep_storage_append_summary(&s_session, &summary);
        strlcpy(s_status.state, "Session saved", sizeof(s_status.state));
    }
    s_status.monitoring = enabled;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void sleep_engine_get_status(sleep_engine_status_t *status)
{
    if (!s_lock) { memset(status, 0, sizeof(*status)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_lock);
}
