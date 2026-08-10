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

static void configure_detector(const app_settings_t *settings)
{
    snore_detector_config_t config = {
        .start_threshold = settings->start_probability / 100.0f,
        .end_threshold = settings->end_probability / 100.0f,
        .start_frames = 2,
        .end_frames = settings->model_profile == APP_MODEL_CONSERVATIVE ? 4 : 3,
        .minimum_event_ms = 1500,
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
                strlcpy(s_status.state, s_detector.active ? "Snoring event" :
                        (s_status.recording_audio ? "Listening + recording" : "Listening"),
                        sizeof(s_status.state));
                xSemaphoreGive(s_lock);
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
    snore_classifier_set_profile(settings.model_profile);
    configure_detector(&settings);
    s_status.model_profile = settings.model_profile;
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
    if (enabled) {
        esp_err_t model_err = snore_classifier_set_profile(settings.model_profile);
        if (model_err != ESP_OK) return model_err;
    }
    recorder_status_t recorder;
    audio_recorder_get_status(&recorder);
    if (enabled && !recorder.ready) return ESP_ERR_INVALID_STATE;

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
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (enabled && !s_status.monitoring) {
        memset(&s_session, 0, sizeof(s_session));
        sleep_session_start(&s_session, esp_timer_get_time() / 1000);
        configure_detector(&settings);
        s_filled = 0;
        s_dropped_blocks = 0;
        xQueueReset(s_queue);
        s_status.model_profile = settings.model_profile;
        s_status.recording_audio = recorder.recording || s_recording_owned;
        strlcpy(s_status.state, s_status.recording_audio ? "Listening + recording" : "Preparing audio",
                sizeof(s_status.state));
    } else if (!enabled && s_status.monitoring) {
        sleep_summary_t summary;
        sleep_session_finish(&s_session, esp_timer_get_time() / 1000, &summary);
        sleep_storage_append_summary(&s_session, &summary);
        stop_owned_recording = s_recording_owned;
        s_recording_owned = false;
        s_status.recording_audio = false;
        strlcpy(s_status.state, stop_owned_recording ? "Saving night recording" : "Session saved",
                sizeof(s_status.state));
    }
    s_status.monitoring = enabled;
    xSemaphoreGive(s_lock);

    if (stop_owned_recording) {
        recorder_status_t current_recorder = {0};
        audio_recorder_get_status(&current_recorder);
        esp_err_t record_err = current_recorder.recording ? audio_recorder_stop() : ESP_OK;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_status.state, record_err == ESP_OK ? "Session + WAV saved" : "Session saved; WAV error",
                sizeof(s_status.state));
        xSemaphoreGive(s_lock);
        if (record_err != ESP_OK) return record_err;
    }
    return ESP_OK;
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

void sleep_engine_get_status(sleep_engine_status_t *status)
{
    if (!s_lock) { memset(status, 0, sizeof(*status)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_lock);
}
