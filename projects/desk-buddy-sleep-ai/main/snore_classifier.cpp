#include "snore_classifier.h"

#include <math.h>
#include <new>
#include <string.h>
#include "efficientat_mel_weights.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "snore_model";

// EfficientAT AudioSet MN04 frontend: 2 s, 32 kHz, 128 Kaldi Mel bands.
static constexpr size_t SAMPLE_COUNT = 64000;
static constexpr size_t PREEMPHASIS_COUNT = SAMPLE_COUNT - 1;
static constexpr int FFT_SIZE = 1024;
static constexpr int FFT_BINS = FFT_SIZE / 2 + 1;
static constexpr int WINDOW_LENGTH = 800;
static constexpr int WINDOW_PAD = (FFT_SIZE - WINDOW_LENGTH) / 2;
static constexpr int HOP_LENGTH = 320;
static constexpr int FRAME_COUNT = 200;
static constexpr int MEL_BANDS = 128;
static constexpr size_t FEATURE_COUNT = MEL_BANDS * FRAME_COUNT;
static constexpr size_t ARENA_BYTES = 3 * 1024 * 1024;

extern const uint8_t snoring_model_start[]
    asm("_binary_efficientat_mn04_snoring_int8_tflite_start");
extern const uint8_t snoring_model_end[]
    asm("_binary_efficientat_mn04_snoring_int8_tflite_end");

static uint8_t *s_arena;
static tflite::MicroInterpreter *s_interpreter;
static TfLiteTensor *s_input;
static TfLiteTensor *s_output;
static bool s_available;
static uint8_t s_profile;

static float s_twiddle_cos[FFT_SIZE / 2];
static float s_twiddle_sin[FFT_SIZE / 2];
static float s_hann[WINDOW_LENGTH];
static float *s_preemphasis;
static float *s_fft_real;
static float *s_fft_imag;
static float *s_power;

static void *dsp_calloc(size_t count, size_t size)
{
    void *memory = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory ? memory : heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
}

static void init_dsp_tables(void)
{
    for (int index = 0; index < FFT_SIZE / 2; ++index) {
        const float angle = -2.0f * (float)M_PI * index / FFT_SIZE;
        s_twiddle_cos[index] = cosf(angle);
        s_twiddle_sin[index] = sinf(angle);
    }
    // torch.hann_window(800, periodic=False)
    for (int index = 0; index < WINDOW_LENGTH; ++index) {
        s_hann[index] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * index /
                                           (WINDOW_LENGTH - 1));
    }
}

static void fft1024(float *real, float *imag)
{
    for (unsigned index = 1, reversed = 0; index < FFT_SIZE; ++index) {
        unsigned bit = FFT_SIZE >> 1;
        for (; reversed & bit; bit >>= 1) reversed ^= bit;
        reversed ^= bit;
        if (index < reversed) {
            float value = real[index]; real[index] = real[reversed]; real[reversed] = value;
            value = imag[index]; imag[index] = imag[reversed]; imag[reversed] = value;
        }
    }
    for (unsigned length = 2; length <= FFT_SIZE; length <<= 1) {
        const unsigned half = length >> 1;
        const unsigned step = FFT_SIZE / length;
        for (unsigned base = 0; base < FFT_SIZE; base += length) {
            for (unsigned offset = 0; offset < half; ++offset) {
                const unsigned twiddle = offset * step;
                const float wr = s_twiddle_cos[twiddle];
                const float wi = s_twiddle_sin[twiddle];
                const unsigned even = base + offset;
                const unsigned odd = even + half;
                const float tr = wr * real[odd] - wi * imag[odd];
                const float ti = wr * imag[odd] + wi * real[odd];
                real[odd] = real[even] - tr;
                imag[odd] = imag[even] - ti;
                real[even] += tr;
                imag[even] += ti;
            }
        }
    }
}

// torch.stft(center=True) uses reflection padding that excludes the edge.
static size_t torch_reflect_index(int index, int length)
{
    while (index < 0 || index >= length) {
        if (index < 0) index = -index;
        if (index >= length) index = 2 * length - 2 - index;
    }
    return (size_t)index;
}

static float amplitude_dbfs(const int16_t *pcm)
{
    double energy = 0.0;
    for (size_t index = 0; index < SAMPLE_COUNT; ++index) {
        const float value = pcm[index] / 32768.0f;
        energy += value * value;
    }
    const float rms = sqrtf((float)(energy / SAMPLE_COUNT));
    return rms > 1e-8f ? 20.0f * log10f(rms) : -160.0f;
}

static void extract_features(const int16_t *pcm, TfLiteTensor *input)
{
    for (size_t index = 0; index < PREEMPHASIS_COUNT; ++index) {
        s_preemphasis[index] = (pcm[index + 1] - 0.97f * pcm[index]) / 32768.0f;
    }

    const float input_scale = input->params.scale;
    const int input_zero = input->params.zero_point;
    for (int frame = 0; frame < FRAME_COUNT; ++frame) {
        memset(s_fft_real, 0, FFT_SIZE * sizeof(float));
        memset(s_fft_imag, 0, FFT_SIZE * sizeof(float));
        const int first = frame * HOP_LENGTH - FFT_SIZE / 2;
        for (int window_index = 0; window_index < WINDOW_LENGTH; ++window_index) {
            const int source_index = first + WINDOW_PAD + window_index;
            s_fft_real[WINDOW_PAD + window_index] =
                s_preemphasis[torch_reflect_index(source_index, PREEMPHASIS_COUNT)] *
                s_hann[window_index];
        }
        fft1024(s_fft_real, s_fft_imag);
        for (int bin = 0; bin < FFT_BINS; ++bin) {
            const float real = s_fft_real[bin];
            const float imag = s_fft_imag[bin];
            s_power[bin] = real * real + imag * imag;
        }

        for (int band = 0; band < MEL_BANDS; ++band) {
            float mel_power = 0.0f;
            const int first_weight = kEfficientAtMelOffsets[band];
            const int last_weight = kEfficientAtMelOffsets[band + 1];
            const int first_bin = kEfficientAtMelStarts[band];
            for (int weight = first_weight; weight < last_weight; ++weight) {
                mel_power += kEfficientAtMelWeights[weight] *
                             s_power[first_bin + weight - first_weight];
            }
            const float normalized = (logf(mel_power + 0.00001f) + 4.5f) / 5.0f;
            int quantized = (int)lrintf(normalized / input_scale) + input_zero;
            if (quantized < -128) quantized = -128;
            if (quantized > 127) quantized = 127;
            // The converted TFLite tensor is NHWC [1, Mel, Time, 1].
            input->data.int8[band * FRAME_COUNT + frame] = (int8_t)quantized;
        }
    }
}

extern "C" esp_err_t snore_classifier_init(void)
{
    s_preemphasis = (float *)dsp_calloc(PREEMPHASIS_COUNT, sizeof(float));
    s_fft_real = (float *)dsp_calloc(FFT_SIZE, sizeof(float));
    s_fft_imag = (float *)dsp_calloc(FFT_SIZE, sizeof(float));
    s_power = (float *)dsp_calloc(FFT_BINS, sizeof(float));
    s_arena = (uint8_t *)dsp_calloc(ARENA_BYTES, 1);
    if (!s_preemphasis || !s_fft_real || !s_fft_imag || !s_power || !s_arena) {
        return ESP_ERR_NO_MEM;
    }
    init_dsp_tables();

    static tflite::MicroMutableOpResolver<11> resolver;
    resolver.AddPad();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddHardSwish();
    resolver.AddFullyConnected();
    resolver.AddAdd();
    resolver.AddMean();
    resolver.AddLogistic();
    resolver.AddMul();
    resolver.AddReshape();
    resolver.AddReduceMax();

    const tflite::Model *model = tflite::GetModel(snoring_model_start);
    if (model->version() != TFLITE_SCHEMA_VERSION) return ESP_ERR_INVALID_VERSION;
    s_interpreter = new (std::nothrow) tflite::MicroInterpreter(
        model, resolver, s_arena, ARENA_BYTES);
    if (!s_interpreter) return ESP_ERR_NO_MEM;
    if (s_interpreter->AllocateTensors() != kTfLiteOk) return ESP_FAIL;
    s_input = s_interpreter->input(0);
    s_output = s_interpreter->output(0);
    if (s_input->type != kTfLiteInt8 || s_output->type != kTfLiteInt8 ||
        s_input->bytes != FEATURE_COUNT || s_output->bytes != 1) {
        ESP_LOGE(TAG, "Tensor mismatch: input=%u output=%u",
                 (unsigned)s_input->bytes, (unsigned)s_output->bytes);
        return ESP_ERR_INVALID_SIZE;
    }
    s_available = true;
    ESP_LOGI(TAG, "EfficientAT Snoring: %u-byte model, %u-byte arena, input %.7f/%ld",
             (unsigned)(snoring_model_end - snoring_model_start),
             (unsigned)s_interpreter->arena_used_bytes(), s_input->params.scale,
             (long)s_input->params.zero_point);
    return ESP_OK;
}

extern "C" esp_err_t snore_classifier_set_profile(uint8_t profile)
{
    if (profile > 1 || !s_available) return ESP_ERR_INVALID_ARG;
    s_profile = profile;
    ESP_LOGI(TAG, "Snoring sensitivity: %s", profile == 0 ? "balanced" : "sensitive");
    return ESP_OK;
}

extern "C" void snore_classifier_get_info(snore_classifier_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->model_available = s_available;
    info->model_name = s_available ? "EfficientAT AudioSet Snoring" : "No model installed";
    info->sample_rate = 32000;
    info->window_samples = SAMPLE_COUNT;
}

extern "C" esp_err_t snore_classifier_predict(const int16_t *samples, size_t count,
                                                float *probability)
{
    if (!s_available) return ESP_ERR_NOT_SUPPORTED;
    if (!samples || count != SAMPLE_COUNT || !probability) return ESP_ERR_INVALID_ARG;
    extract_features(samples, s_input);
    if (s_interpreter->Invoke() != kTfLiteOk) return ESP_FAIL;
    const float logit = (s_output->data.int8[0] - s_output->params.zero_point) *
                        s_output->params.scale;
    *probability = 1.0f / (1.0f + expf(-logit));
    static unsigned prediction_count;
    if (++prediction_count == 1 || prediction_count % 10 == 0) {
        ESP_LOGI(TAG, "Snoring %.1f%%, audio %.1f dBFS, inference profile %u",
                 *probability * 100.0f, amplitude_dbfs(samples), s_profile);
    }
    return ESP_OK;
}
