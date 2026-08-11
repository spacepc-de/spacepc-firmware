#include "audio_recorder.h"

#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wav_writer.h"

#define BLOCK_FRAMES 512
#define INPUT_CHANNELS 2
#define SEGMENT_SECONDS (30U * 60U)
#define MICROPHONE_GAIN_DB 36U

static const char *TAG = "audio_recorder";
static esp_codec_dev_handle_t s_mic;
static SemaphoreHandle_t s_lock;
static recorder_status_t s_status;
static wav_writer_t s_writer;
static int16_t s_stereo[BLOCK_FRAMES * INPUT_CHANNELS];
static uint64_t s_recorded_samples;
static uint32_t s_segment_samples;
static audio_pcm_callback_t s_pcm_callback;
static void *s_pcm_context;

static void set_error(const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_status.error, message, sizeof(s_status.error));
    xSemaphoreGive(s_lock);
    ESP_LOGE(TAG, "%s", message);
}

static bool next_filename(char *path, size_t size)
{
    struct stat st;
    for (unsigned i = 1; i <= 9999; ++i) {
        snprintf(path, size, BSP_SD_MOUNT_POINT "/ST%04u.WAV", i);
        if (stat(path, &st) != 0) return true;
    }
    return false;
}

static void recorder_task(void *arg)
{
    (void)arg;
    while (true) {
        int ret = esp_codec_dev_read(s_mic, s_stereo, sizeof(s_stereo));
        if (ret != ESP_CODEC_DEV_OK) {
            set_error("Microphone read failed");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        double energy[INPUT_CHANNELS] = {0};
        int peak[INPUT_CHANNELS] = {0};
        for (size_t i = 0; i < BLOCK_FRAMES; ++i) {
            for (size_t channel = 0; channel < INPUT_CHANNELS; ++channel) {
                int sample = s_stereo[i * INPUT_CHANNELS + channel];
                energy[channel] += (double)sample * sample;
                int magnitude = sample < 0 ? -sample : sample;
                if (magnitude > peak[channel]) peak[channel] = magnitude;
            }
        }
        float rms_dbfs[INPUT_CHANNELS];
        float peak_dbfs[INPUT_CHANNELS];
        for (size_t channel = 0; channel < INPUT_CHANNELS; ++channel) {
            float rms = sqrtf((float)(energy[channel] / BLOCK_FRAMES));
            rms_dbfs[channel] = rms > 0 ? 20.0f * log10f(rms / 32768.0f) : -96.0f;
            peak_dbfs[channel] = peak[channel] > 0 ? 20.0f * log10f(peak[channel] / 32768.0f) : -96.0f;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.rms_dbfs = rms_dbfs[0];
        s_status.peak_dbfs = peak_dbfs[0];
        s_status.rms_dbfs_ch2 = rms_dbfs[1];
        s_status.peak_dbfs_ch2 = peak_dbfs[1];
        if (s_status.recording) {
            if (!wav_writer_write(&s_writer, s_stereo, BLOCK_FRAMES * INPUT_CHANNELS)) {
                s_status.recording = false;
                s_status.dropped_blocks++;
                strlcpy(s_status.error, "SD write failed; recording stopped", sizeof(s_status.error));
                wav_writer_close(&s_writer, RECORDER_SAMPLE_RATE, INPUT_CHANNELS);
            } else {
                s_recorded_samples += BLOCK_FRAMES;
                s_segment_samples += BLOCK_FRAMES;
                s_status.elapsed_seconds = s_recorded_samples / RECORDER_SAMPLE_RATE;
                if (s_segment_samples >= RECORDER_SAMPLE_RATE * SEGMENT_SECONDS) {
                    bool closed = wav_writer_close(&s_writer, RECORDER_SAMPLE_RATE, INPUT_CHANNELS);
                    char next_path[sizeof(s_status.filename)];
                    if (!closed || !next_filename(next_path, sizeof(next_path)) ||
                        !wav_writer_open(&s_writer, next_path, RECORDER_SAMPLE_RATE, INPUT_CHANNELS)) {
                        s_status.recording = false;
                        s_status.dropped_blocks++;
                        strlcpy(s_status.error, "Cannot start next 30-minute WAV segment", sizeof(s_status.error));
                    } else {
                        s_segment_samples = 0;
                        strlcpy(s_status.filename, next_path, sizeof(s_status.filename));
                        ESP_LOGI(TAG, "Continuing overnight recording in %s", next_path);
                    }
                }
            }
        }
        xSemaphoreGive(s_lock);
        if (s_pcm_callback) s_pcm_callback(s_stereo, BLOCK_FRAMES, s_pcm_context);
    }
}

void audio_recorder_set_pcm_callback(audio_pcm_callback_t callback, void *context)
{
    s_pcm_context = context;
    s_pcm_callback = callback;
}

esp_err_t audio_recorder_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    esp_err_t sd_err = bsp_sdcard_mount();
    if (sd_err != ESP_OK) {
        set_error("Insert a FAT32 microSD card and restart");
        s_status.sd_mounted = false;
    } else {
        s_status.sd_mounted = true;
    }

    i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RECORDER_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK, .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT, .din = BSP_I2S_DSIN,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&i2s_config), TAG, "I2S init");
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) return ESP_FAIL;

    esp_codec_dev_sample_info_t format = {
        .sample_rate = RECORDER_SAMPLE_RATE,
        .channel = INPUT_CHANNELS,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_mic, &format) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    if (esp_codec_dev_set_in_gain(s_mic, MICROPHONE_GAIN_DB) != ESP_CODEC_DEV_OK) return ESP_FAIL;

    s_status.ready = true;
    s_status.microphone_gain_db = MICROPHONE_GAIN_DB;
    s_status.rms_dbfs = -96.0f;
    s_status.peak_dbfs = -96.0f;
    s_status.rms_dbfs_ch2 = -96.0f;
    s_status.peak_dbfs_ch2 = -96.0f;
    if (xTaskCreatePinnedToCore(recorder_task, "audio_capture", 6144, NULL, 12, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_recorder_start(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool allowed = s_status.ready && s_status.sd_mounted && !s_status.recording;
    xSemaphoreGive(s_lock);
    if (!allowed) return ESP_ERR_INVALID_STATE;

    char path[sizeof(s_status.filename)];
    if (!next_filename(path, sizeof(path))) return ESP_ERR_NOT_FOUND;
    if (!wav_writer_open(&s_writer, path, RECORDER_SAMPLE_RATE, INPUT_CHANNELS)) {
        char message[96];
        snprintf(message, sizeof(message), "Cannot create WAV: %s", strerror(errno));
        set_error(message);
        return ESP_FAIL;
    }

    s_recorded_samples = 0;
    s_segment_samples = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_status.filename, path, sizeof(s_status.filename));
    s_status.elapsed_seconds = 0;
    s_status.error[0] = '\0';
    s_status.recording = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Recording %s", path);
    return ESP_OK;
}

esp_err_t audio_recorder_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_recording = s_status.recording;
    s_status.recording = false;
    bool ok = was_recording && wav_writer_close(&s_writer, RECORDER_SAMPLE_RATE, INPUT_CHANNELS);
    xSemaphoreGive(s_lock);
    if (!was_recording) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "Recording finalized (%lu bytes)", (unsigned long)s_writer.data_bytes);
    return ok ? ESP_OK : ESP_FAIL;
}

void audio_recorder_get_status(recorder_status_t *status)
{
    if (!s_lock) {
        memset(status, 0, sizeof(*status));
        status->rms_dbfs = -96.0f;
        status->peak_dbfs = -96.0f;
        status->rms_dbfs_ch2 = -96.0f;
        status->peak_dbfs_ch2 = -96.0f;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_lock);
}
