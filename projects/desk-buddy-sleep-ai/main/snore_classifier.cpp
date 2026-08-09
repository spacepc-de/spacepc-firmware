#include "snore_classifier.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "snore_model";
static const char *MODEL_PATH = BSP_SD_MOUNT_POINT "/SNORE.TFL";
static constexpr size_t ARENA_BYTES = 2 * 1024 * 1024;
static uint8_t *s_model_data;
static uint8_t *s_arena;
static size_t s_model_size;
static const tflite::Model *s_model;
static tflite::MicroInterpreter *s_interpreter;
static TfLiteTensor *s_input;
static TfLiteTensor *s_output;
static bool s_available;
static constexpr int FEATURE_FRAMES = 8;
static constexpr int FEATURE_BANDS = 24;
static constexpr int FRAME_SAMPLES = 2000;
static constexpr int FEATURE_COUNT = FEATURE_FRAMES * (FEATURE_BANDS + 2);

static void extract_features(const int16_t *samples, float *features)
{
    for (int frame = 0; frame < FEATURE_FRAMES; ++frame) {
        const int16_t *pcm = samples + frame * FRAME_SAMPLES;
        double energy = 1e-9;
        int crossings = 0;
        for (int i = 0; i < FRAME_SAMPLES; ++i) {
            float sample = pcm[i] / 32768.0f;
            energy += sample * sample;
            if (i && ((pcm[i] < 0) != (pcm[i - 1] < 0))) crossings++;
        }
        int offset = frame * (FEATURE_BANDS + 2);
        for (int band = 0; band < FEATURE_BANDS; ++band) {
            float frequency = 80.0f * powf(6000.0f / 80.0f, band / (float)(FEATURE_BANDS - 1));
            float coefficient = 2.0f * cosf(2.0f * (float)M_PI * frequency / 16000.0f);
            float q0 = 0, q1 = 0, q2 = 0;
            for (int i = 0; i < FRAME_SAMPLES; ++i) {
                q0 = coefficient * q1 - q2 + pcm[i] / 32768.0f;
                q2 = q1; q1 = q0;
            }
            float power = q1 * q1 + q2 * q2 - coefficient * q1 * q2;
            float ratio = power / ((float)energy * FRAME_SAMPLES);
            float value = (log10f(ratio + 1e-8f) + 8.0f) / 8.0f;
            features[offset + band] = value < 0 ? 0 : value > 1 ? 1 : value;
        }
        float rms = sqrtf((float)energy / FRAME_SAMPLES);
        float level = (20.0f * log10f(rms + 1e-6f) + 80.0f) / 80.0f;
        features[offset + FEATURE_BANDS] = level < 0 ? 0 : level > 1 ? 1 : level;
        features[offset + FEATURE_BANDS + 1] = crossings / (float)(FRAME_SAMPLES - 1);
    }
}

static bool load_file(void)
{
    FILE *file = fopen(MODEL_PATH, "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size <= 0 || size > 4 * 1024 * 1024) { fclose(file); return false; }
    s_model_data = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_model_data) { fclose(file); return false; }
    bool ok = fread(s_model_data, 1, size, file) == (size_t)size;
    fclose(file);
    if (!ok) { heap_caps_free(s_model_data); s_model_data = nullptr; return false; }
    s_model_size = (size_t)size;
    return true;
}

extern "C" esp_err_t snore_classifier_init(void)
{
    if (!load_file()) {
        ESP_LOGW(TAG, "%s not found", MODEL_PATH);
        return ESP_ERR_NOT_FOUND;
    }
    s_model = tflite::GetModel(s_model_data);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) return ESP_ERR_INVALID_VERSION;
    s_arena = (uint8_t *)heap_caps_malloc(ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_arena) return ESP_ERR_NO_MEM;

    static tflite::MicroMutableOpResolver<9> resolver;
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddMaxPool2D();
    resolver.AddMean();
    resolver.AddFullyConnected();
    resolver.AddLogistic();
    resolver.AddReshape();
    resolver.AddQuantize();
    resolver.AddDequantize();
    static tflite::MicroInterpreter interpreter(s_model, resolver, s_arena, ARENA_BYTES);
    s_interpreter = &interpreter;
    if (s_interpreter->AllocateTensors() != kTfLiteOk) return ESP_FAIL;
    s_input = s_interpreter->input(0);
    s_output = s_interpreter->output(0);
    if (s_input->type != kTfLiteInt8 || s_output->type != kTfLiteInt8 ||
        s_input->bytes != FEATURE_COUNT || s_output->bytes != 1) {
        ESP_LOGE(TAG, "Expected int8 [%d] input and scalar output", FEATURE_COUNT);
        return ESP_ERR_INVALID_SIZE;
    }
    s_available = true;
    ESP_LOGI(TAG, "Loaded %u-byte model; arena used %u bytes", (unsigned)s_model_size,
             (unsigned)s_interpreter->arena_used_bytes());
    return ESP_OK;
}

extern "C" void snore_classifier_get_info(snore_classifier_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->model_available = s_available;
    info->model_name = s_available ? "Snore AI v1" : "No model installed";
    info->sample_rate = 16000;
    info->window_samples = 16000;
}

extern "C" esp_err_t snore_classifier_predict(const int16_t *samples, size_t count, float *probability)
{
    if (!s_available) return ESP_ERR_NOT_SUPPORTED;
    if (!samples || count != 16000 || !probability) return ESP_ERR_INVALID_ARG;
    float features[FEATURE_COUNT];
    extract_features(samples, features);
    const float scale = s_input->params.scale;
    const int zero = s_input->params.zero_point;
    for (size_t i = 0; i < FEATURE_COUNT; ++i) {
        int value = (int)lrintf(features[i] / scale) + zero;
        s_input->data.int8[i] = (int8_t)(value < -128 ? -128 : value > 127 ? 127 : value);
    }
    if (s_interpreter->Invoke() != kTfLiteOk) return ESP_FAIL;
    *probability = (s_output->data.int8[0] - s_output->params.zero_point) * s_output->params.scale;
    if (*probability < 0) *probability = 0;
    if (*probability > 1) *probability = 1;
    return ESP_OK;
}
