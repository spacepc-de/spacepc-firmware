#include "wifi_manager.h"

#include <string.h>
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "wifi";
static bool initialized;
static volatile bool connected;
static volatile bool has_credentials;
static bool sntp_started;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (has_credentials) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        if (has_credentials) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        connected = true;
        if (!sntp_started) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            sntp_started = true;
        }
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Connected, IP " IPSTR, IP2STR(&event->ip_info.ip));
    }
    (void)arg;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!initialized || !ssid || !ssid[0]) return ESP_ERR_INVALID_STATE;
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password ? password : "", sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    connected = false;
    has_credentials = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) return err;
    esp_wifi_disconnect();
    return esp_wifi_connect();
}

esp_err_t wifi_manager_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL), TAG, "wifi event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL), TAG, "ip event");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "station mode");
    initialized = true;

    char ssid[33] = {0};
    char password[65] = {0};
    nvs_handle_t nvs;
    if (nvs_open("air-ui", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(ssid);
        nvs_get_str(nvs, "ssid", ssid, &size);
        size = sizeof(password);
        nvs_get_str(nvs, "password", password, &size);
        nvs_close(nvs);
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    if (ssid[0]) return wifi_manager_connect(ssid, password);
    ESP_LOGI(TAG, "No saved WiFi credentials");
    return ESP_OK;
}

bool wifi_manager_connected(void)
{
    return connected;
}
