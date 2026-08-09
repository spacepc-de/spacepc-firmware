#include <string.h>
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sen66.h"
#include "spacepc_api.h"
#include "ui.h"
#include "wifi_manager.h"

static const char *TAG = "air_dashboard";

void app_main(void)
{
    bsp_display_cfg_t display_config = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL,
        /* GT911 is physically portrait while LVGL is rotated to landscape. */
        .touch_flags = {.swap_xy = 1, .mirror_x = 1, .mirror_y = 0},
    };

    ESP_LOGI(TAG, "Starting 800x480 SEN66 dashboard");
    esp_err_t nvs_status = nvs_flash_init();
    if (nvs_status == ESP_ERR_NVS_NO_FREE_PAGES || nvs_status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    bsp_display_start_with_config(&display_config);
    bsp_display_backlight_on();
    bsp_display_lock(-1);
    air_ui_create();
    bsp_display_unlock();
    /* Start the SEN66 before powering up the C6 radio. On a cold boot the
     * sensor fan and WiFi association otherwise create their current peaks at
     * the same time, which can brown out USB-powered boards. */
    esp_err_t sensor_status = sen66_init(bsp_i2c_get_handle());
    ESP_LOGI(TAG, "SEN66 init: %s", esp_err_to_name(sensor_status));
    /* Let the fan inrush and the 5 V rail settle before the C6 radio starts. */
    vTaskDelay(pdMS_TO_TICKS(2500));
    /* Relieve the shared supply only while the C6 performs its high-current
     * association burst. The regular UI always runs at full brightness. */
    bsp_display_brightness_set(30);
    esp_err_t wifi_status = wifi_manager_init();
    ESP_LOGI(TAG, "WiFi init: %s", esp_err_to_name(wifi_status));
    /* Initialize mDNS before GOT_IP so it observes and enables the remote STA
     * netif as soon as association completes. */
    esp_err_t api_status = spacepc_api_start();
    ESP_LOGI(TAG, "SpacePC API init: %s", esp_err_to_name(api_status));
    for (int i = 0; i < 80 && !wifi_manager_connected(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    bsp_display_brightness_set(100);
    unsigned age = 0;
    unsigned failures = 0;
    sen66_data_t data;
    memset(&data, 0, sizeof(data));

    while (true) {
        if (sensor_status != ESP_OK && failures > 0 && failures % 5 == 0) {
            sensor_status = sen66_init(bsp_i2c_get_handle());
            ESP_LOGI(TAG, "SEN66 retry: %s", esp_err_to_name(sensor_status));
        }
        sen66_data_t latest;
        esp_err_t err = sensor_status == ESP_OK ? sen66_read(&latest) : sensor_status;
        bool connected = err == ESP_OK;
        if (connected) {
            data = latest;
            age = 0;
            failures = 0;
            ESP_LOGI(TAG, "CO2 %.0f ppm, PM2.5 %.1f ug/m3, T %.1f C, RH %.0f%%",
                     data.co2, data.pm25, data.temperature, data.humidity);
        } else {
            ++age;
            ++failures;
            if (failures == 1 || failures % 10 == 0) ESP_LOGW(TAG, "SEN66 read: %s", esp_err_to_name(err));
        }

        spacepc_api_update(connected ? &data : NULL, connected);

        esp_err_t ui_lock = bsp_display_lock(1000);
        if (ui_lock == ESP_OK) {
            air_ui_update(connected ? &data : NULL, connected, age);
            bsp_display_unlock();
        } else {
            ESP_LOGE(TAG, "LVGL lock failed: %s", esp_err_to_name(ui_lock));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
