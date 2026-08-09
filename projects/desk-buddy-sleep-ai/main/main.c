#include "audio_recorder.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_log.h"
#include "recorder_ui.h"

void app_main(void)
{
    bsp_display_cfg_t display_config = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL,
        .touch_flags = {.swap_xy = 1, .mirror_x = 1, .mirror_y = 0},
    };
    bsp_display_start_with_config(&display_config);
    bsp_display_backlight_on();

    bsp_display_lock(-1);
    recorder_ui_create();
    bsp_display_unlock();

    esp_err_t err = audio_recorder_init();
    ESP_LOGI("sleep_ai", "Audio recorder: %s", esp_err_to_name(err));
}
