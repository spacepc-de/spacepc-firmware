#include <string.h>
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_log.h"
#include "orb_data.h"
#include "orb_geocode.h"
#include "orb_map.h"
#include "orb_route.h"
#include "orb_settings.h"
#include "orb_timezone.h"
#include "orb_ui.h"
#include "orb_wifi.h"

static const char *TAG = "orb";

void app_main(void)
{
    static orb_settings_t settings;
    esp_err_t err = orb_settings_load(&settings);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Settings unavailable: %s", esp_err_to_name(err));
        orb_settings_defaults(&settings);
    }

    bsp_display_cfg_t display_config = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL,
        .touch_flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    bsp_display_start_with_config(&display_config);
    bsp_display_backlight_on();
    bsp_display_brightness_set(100);

    err = orb_timezone_init();
    ESP_LOGI(TAG, "Timezone lookup: %s", esp_err_to_name(err));

    err = orb_map_init();
    ESP_LOGI(TAG, "Map engine: %s", esp_err_to_name(err));

    err = orb_data_init(&settings);
    ESP_LOGI(TAG, "Live data engine: %s", esp_err_to_name(err));

    err = orb_geocode_init();
    ESP_LOGI(TAG, "City lookup: %s", esp_err_to_name(err));

    err = orb_route_init();
    ESP_LOGI(TAG, "Flight routes: %s", esp_err_to_name(err));

    bsp_display_lock(-1);
    err = orb_ui_create(&settings);
    bsp_display_unlock();
    ESP_LOGI(TAG, "UI: %s", esp_err_to_name(err));

    err = orb_wifi_init(settings.wifi_ssid, settings.wifi_password);
    ESP_LOGI(TAG, "WiFi: %s", esp_err_to_name(err));
    if (settings.city[0] && strcmp(settings.timezone, "UTC") == 0) {
        if (!orb_geocode_request(settings.city)) ESP_LOGW(TAG, "Could not refresh saved city timezone");
    } else if (!orb_timezone_request(settings.latitude, settings.longitude)) {
        ESP_LOGW(TAG, "Could not queue initial timezone lookup");
    }
}
