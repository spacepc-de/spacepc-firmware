#include "app_settings.h"

#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define SETTINGS_VERSION 7

typedef struct {
    uint16_t version;
    app_settings_t values;
} settings_blob_t;

void app_settings_defaults(app_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->start_hour = 23;
    settings->end_hour = 7;
    settings->night_brightness = 8;
    settings->microphone_channel = 2;
    settings->reserved_microphone_gain_db = 36;
    settings->start_probability = 50;
    settings->end_probability = 25;
    settings->reserved_model_profile = 0;
    settings->schedule_enabled = true;
    settings->record_during_monitoring = true;
    settings->timezone_index = 0;
}

esp_err_t app_settings_load(app_settings_t *settings)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    nvs_handle_t handle;
    err = nvs_open("sleep_ai", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        app_settings_defaults(settings);
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    settings_blob_t blob;
    size_t size = sizeof(blob);
    err = nvs_get_blob(handle, "settings", &blob, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(blob) || blob.version != SETTINGS_VERSION) {
        app_settings_defaults(settings);
        return ESP_OK;
    }
    *settings = blob.values;
    return ESP_OK;
}

esp_err_t app_settings_save(const app_settings_t *settings)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("sleep_ai", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    settings_blob_t blob = {.version = SETTINGS_VERSION, .values = *settings};
    err = nvs_set_blob(handle, "settings", &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
