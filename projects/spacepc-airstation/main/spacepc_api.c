#include "spacepc_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "ui.h"

static const char *TAG = "spacepc_api";
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static sen66_data_t latest;
static bool sensor_available;
static char device_id[24];

static const char *entity_info =
    "[{\"id\":\"co2\",\"name\":\"Carbon dioxide\",\"platform\":\"sensor\",\"device_class\":\"carbon_dioxide\",\"state_class\":\"measurement\",\"unit\":\"ppm\"},"
    "{\"id\":\"pm1\",\"name\":\"PM1.0\",\"platform\":\"sensor\",\"device_class\":\"pm1\",\"state_class\":\"measurement\",\"unit\":\"µg/m³\"},"
    "{\"id\":\"pm25\",\"name\":\"PM2.5\",\"platform\":\"sensor\",\"device_class\":\"pm25\",\"state_class\":\"measurement\",\"unit\":\"µg/m³\"},"
    "{\"id\":\"pm4\",\"name\":\"PM4.0\",\"platform\":\"sensor\",\"state_class\":\"measurement\",\"unit\":\"µg/m³\"},"
    "{\"id\":\"pm10\",\"name\":\"PM10\",\"platform\":\"sensor\",\"device_class\":\"pm10\",\"state_class\":\"measurement\",\"unit\":\"µg/m³\"},"
    "{\"id\":\"temperature\",\"name\":\"Temperature\",\"platform\":\"sensor\",\"device_class\":\"temperature\",\"state_class\":\"measurement\",\"unit\":\"°C\"},"
    "{\"id\":\"humidity\",\"name\":\"Humidity\",\"platform\":\"sensor\",\"device_class\":\"humidity\",\"state_class\":\"measurement\",\"unit\":\"%\"},"
    "{\"id\":\"voc_index\",\"name\":\"VOC index\",\"platform\":\"sensor\",\"state_class\":\"measurement\",\"unit\":\"index\"},"
    "{\"id\":\"nox_index\",\"name\":\"NOx index\",\"platform\":\"sensor\",\"state_class\":\"measurement\",\"unit\":\"index\"},"
    "{\"id\":\"display_power\",\"name\":\"Display\",\"platform\":\"switch\"}]";

static esp_err_t send_json(httpd_req_t *request, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(request, "application/json");
    esp_err_t err = httpd_resp_sendstr(request, json);
    free(json);
    return err;
}

static esp_err_t info_handler(httpd_req_t *request)
{
    const size_t capacity = 4096;
    char *json = malloc(capacity);
    if (!json) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "{\"error\":\"out_of_memory\"}");
    }
    snprintf(json, capacity,
        "{\"api_version\":1,\"device_id\":\"%s\",\"name\":\"Airstation\","
        "\"manufacturer\":\"SpacePC\",\"model\":\"Waveshare ESP32-P4 4.3 + SEN66\","
        "\"project_id\":\"spacepc-airstation\","
        "\"firmware\":{\"version\":\"1.0.0\",\"build_date\":\"%s\",\"source_commit\":null},"
        "\"auth_required\":false,\"entities\":%s}", device_id, __DATE__, entity_info);
    httpd_resp_set_type(request, "application/json");
    esp_err_t err = httpd_resp_sendstr(request, json);
    free(json);
    return err;
}

static void add_sensor(cJSON *entities, const char *id, float value, bool available)
{
    cJSON *state = cJSON_AddObjectToObject(entities, id);
    if (available) cJSON_AddNumberToObject(state, "value", value);
    else cJSON_AddNullToObject(state, "value");
    cJSON_AddBoolToObject(state, "available", available);
}

static esp_err_t state_handler(httpd_req_t *request)
{
    sen66_data_t data;
    bool available;
    portENTER_CRITICAL(&state_lock);
    data = latest;
    available = sensor_available;
    portEXIT_CRITICAL(&state_lock);

    cJSON *root = cJSON_CreateObject();
    cJSON *entities = cJSON_AddObjectToObject(root, "entities");
    add_sensor(entities, "co2", data.co2, available);
    add_sensor(entities, "pm1", data.pm1, available);
    add_sensor(entities, "pm25", data.pm25, available);
    add_sensor(entities, "pm4", data.pm4, available);
    add_sensor(entities, "pm10", data.pm10, available);
    add_sensor(entities, "temperature", data.temperature, available);
    add_sensor(entities, "humidity", data.humidity, available);
    add_sensor(entities, "voc_index", data.voc, available);
    add_sensor(entities, "nox_index", data.nox, available);
    cJSON *display = cJSON_AddObjectToObject(entities, "display_power");
    cJSON_AddBoolToObject(display, "on", air_ui_display_enabled());
    cJSON_AddBoolToObject(display, "available", true);

    cJSON *diagnostics = cJSON_AddObjectToObject(root, "diagnostics");
    cJSON_AddNumberToObject(diagnostics, "uptime_seconds", esp_timer_get_time() / 1000000);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        cJSON_AddNumberToObject(diagnostics, "wifi_rssi_dbm", ap.rssi);
    cJSON_AddNumberToObject(diagnostics, "free_heap_bytes", esp_get_free_heap_size());
    return send_json(request, root);
}

static esp_err_t entity_handler(httpd_req_t *request)
{
    const char *prefix = "/api/v1/entities/";
    const char *entity = request->uri + strlen(prefix);
    if (strcmp(entity, "display_power") != 0) {
        httpd_resp_set_status(request, "404 Not Found");
        return httpd_resp_sendstr(request, "{\"error\":\"unknown_entity\"}");
    }
    if (request->content_len <= 0 || request->content_len >= 96) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_sendstr(request, "{\"error\":\"invalid_command\"}");
    }
    char body[96] = {0};
    int received = httpd_req_recv(request, body, request->content_len);
    if (received <= 0) return ESP_FAIL;
    cJSON *json = cJSON_ParseWithLength(body, received);
    cJSON *on = json ? cJSON_GetObjectItemCaseSensitive(json, "on") : NULL;
    if (!cJSON_IsBool(on)) {
        if (json) cJSON_Delete(json);
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_sendstr(request, "{\"error\":\"invalid_command\"}");
    }
    air_ui_set_display_enabled(cJSON_IsTrue(on));
    cJSON_Delete(json);
    return httpd_resp_send(request, NULL, 0);
}

esp_err_t spacepc_api_start(void)
{
    uint8_t mac[6];
    /* ESP32-P4 uses the companion C6 for WiFi and therefore has no local
     * ESP_MAC_WIFI_STA address. Its factory base MAC is stable and unique. */
    ESP_RETURN_ON_ERROR(esp_efuse_mac_get_default(mac), TAG, "read base MAC");
    snprintf(device_id, sizeof(device_id), "airstation-%02x%02x%02x", mac[3], mac[4], mac[5]);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "HTTP server");
    const httpd_uri_t info = {.uri = "/api/v1/info", .method = HTTP_GET, .handler = info_handler};
    const httpd_uri_t state = {.uri = "/api/v1/state", .method = HTTP_GET, .handler = state_handler};
    const httpd_uri_t entity = {.uri = "/api/v1/entities/*", .method = HTTP_POST, .handler = entity_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &info), TAG, "info route");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &state), TAG, "state route");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &entity), TAG, "entity route");

    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mDNS init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(device_id), TAG, "mDNS hostname");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set("SpacePC Airstation"), TAG, "mDNS instance");
    ESP_RETURN_ON_ERROR(mdns_service_add("SpacePC Airstation", "_spacepc", "_tcp", 80, NULL, 0), TAG, "mDNS service");
    mdns_txt_item_t txt[] = {{"id", device_id}, {"api", "1"},
        {"project", "spacepc-airstation"}, {"path", "/api/v1"}};
    ESP_RETURN_ON_ERROR(mdns_service_txt_set("_spacepc", "_tcp", txt, 4), TAG, "mDNS TXT");
    ESP_LOGI(TAG, "SpacePC Local API v1: http://%s.local/api/v1", device_id);
    return ESP_OK;
}

void spacepc_api_update(const sen66_data_t *data, bool available)
{
    portENTER_CRITICAL(&state_lock);
    if (data) latest = *data;
    sensor_available = available;
    portEXIT_CRITICAL(&state_lock);
}
