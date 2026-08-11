#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "app_settings.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi_default_config.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "time_zones.h"

static const char *TAG = "sleep_wifi";
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_manager_status_t s_status;
static bool s_has_credentials;
static bool s_sntp_started;

static void time_sync_notification(struct timeval *time_value)
{
    (void)time_value;
    portENTER_CRITICAL(&s_status_lock);
    s_status.time_synced = true;
    portEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(TAG, "NTP time synchronized");
}

static void start_sntp(void)
{
    if (s_sntp_started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification);
    esp_sntp_init();
    s_sntp_started = true;
}

static void wifi_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_has_credentials) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        portENTER_CRITICAL(&s_status_lock);
        s_status.connected = false;
        s_status.ip_address[0] = '\0';
        portEXIT_CRITICAL(&s_status_lock);
        if (s_has_credentials) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        portENTER_CRITICAL(&s_status_lock);
        s_status.connected = true;
        snprintf(s_status.ip_address, sizeof(s_status.ip_address), IPSTR, IP2STR(&event->ip_info.ip));
        portEXIT_CRITICAL(&s_status_lock);
        start_sntp();
        ESP_LOGI(TAG, "Connected, IP " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!s_status.initialized || !ssid || !ssid[0]) return ESP_ERR_INVALID_STATE;
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password ? password : "", sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    s_has_credentials = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) return err;
    esp_wifi_disconnect();
    return esp_wifi_connect();
}

esp_err_t wifi_manager_init(void)
{
    app_settings_t settings;
    app_settings_load(&settings);
    time_zones_apply(settings.timezone_index);
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL),
                        TAG, "wifi event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL),
                        TAG, "ip event");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "station mode");
    s_has_credentials = settings.wifi_ssid[0] != '\0';
    s_status.initialized = true;
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    err = esp_wifi_set_max_tx_power(52);
    if (err != ESP_OK) ESP_LOGW(TAG, "Could not limit WiFi TX power: %s", esp_err_to_name(err));
    if (s_has_credentials) return wifi_manager_connect(settings.wifi_ssid, settings.wifi_password);
    ESP_LOGI(TAG, "No saved WiFi credentials");
    return ESP_OK;
}

void wifi_manager_get_status(wifi_manager_status_t *status)
{
    if (!status) return;
    bool clock_valid = time(NULL) >= 1609459200;
    portENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    status->time_synced = status->time_synced || clock_valid;
}

bool wifi_manager_time_valid(void)
{
    return time(NULL) >= 1609459200;
}

uint64_t wifi_manager_unix_seconds(void)
{
    time_t now = time(NULL);
    return now >= 1609459200 ? (uint64_t)now : 0;
}
