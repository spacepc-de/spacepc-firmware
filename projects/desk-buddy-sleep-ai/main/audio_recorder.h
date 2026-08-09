#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RECORDER_SAMPLE_RATE 16000

typedef struct {
    bool ready;
    bool recording;
    bool sd_mounted;
    float rms_dbfs;
    float peak_dbfs;
    uint32_t elapsed_seconds;
    uint32_t dropped_blocks;
    char filename[64];
    char error[96];
} recorder_status_t;

esp_err_t audio_recorder_init(void);
esp_err_t audio_recorder_start(void);
esp_err_t audio_recorder_stop(void);
void audio_recorder_get_status(recorder_status_t *status);
