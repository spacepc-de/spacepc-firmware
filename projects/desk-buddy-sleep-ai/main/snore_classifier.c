#include "snore_classifier.h"

#include <string.h>

esp_err_t snore_classifier_init(void)
{
    /* A licensed, quantized model will be linked behind this stable API. */
    return ESP_ERR_NOT_FOUND;
}

void snore_classifier_get_info(snore_classifier_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->model_available = false;
    info->model_name = "No model installed";
    info->sample_rate = 16000;
    info->window_samples = 16000;
}

esp_err_t snore_classifier_predict(const int16_t *mono_samples, size_t sample_count,
                                   float *snore_probability)
{
    (void)mono_samples;
    (void)sample_count;
    if (snore_probability) *snore_probability = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
