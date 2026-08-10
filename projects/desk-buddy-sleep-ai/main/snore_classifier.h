#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool model_available;
    const char *model_name;
    uint32_t sample_rate;
    uint32_t window_samples;
} snore_classifier_info_t;

esp_err_t snore_classifier_init(void);
esp_err_t snore_classifier_set_profile(uint8_t profile);
void snore_classifier_get_info(snore_classifier_info_t *info);
esp_err_t snore_classifier_predict(const int16_t *mono_samples, size_t sample_count,
                                   float *snore_probability);

#ifdef __cplusplus
}
#endif
