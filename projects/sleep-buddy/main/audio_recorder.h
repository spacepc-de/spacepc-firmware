#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define RECORDER_SAMPLE_RATE 32000

typedef struct {
    bool ready;
    bool recording;
    bool sd_mounted;
    float rms_dbfs;
    float peak_dbfs;
    float rms_dbfs_ch2;
    float peak_dbfs_ch2;
    uint32_t elapsed_seconds;
    uint32_t dropped_blocks;
    uint8_t microphone_gain_db;
    char filename[64];
    char error[96];
} recorder_status_t;

typedef void (*audio_pcm_callback_t)(const int16_t *stereo_samples, size_t frame_count, void *context);

esp_err_t audio_recorder_init(void);
esp_err_t audio_recorder_start(void);
esp_err_t audio_recorder_stop(void);
void audio_recorder_get_status(recorder_status_t *status);
void audio_recorder_set_pcm_callback(audio_pcm_callback_t callback, void *context);
