#include "orb_timezone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "orb_net.h"
#include "orb_wifi.h"

#define TIMEZONE_RESPONSE_LIMIT (8 * 1024)

typedef struct {
    double latitude;
    double longitude;
} timezone_request_t;

typedef struct {
    char *data;
    size_t size;
    bool failed;
} response_buffer_t;

static const char *TAG = "orb_timezone";
static QueueHandle_t s_requests;
static QueueHandle_t s_results;

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_buffer_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !response || response->failed) return ESP_OK;
    if (response->size + event->data_len + 1 > TIMEZONE_RESPONSE_LIMIT) {
        response->failed = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(response->data + response->size, event->data, event->data_len);
    response->size += event->data_len;
    response->data[response->size] = '\0';
    return ESP_OK;
}

static bool lookup_timezone(const timezone_request_t *request, orb_timezone_result_t *result, bool *retryable)
{
    *retryable = false;
    char url[192];
    snprintf(url, sizeof(url),
             "https://timeapi.io/api/timezone/coordinate?latitude=%.6f&longitude=%.6f",
             request->latitude, request->longitude);

    response_buffer_t response = {
        .data = heap_caps_malloc(TIMEZONE_RESPONSE_LIMIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    if (!response.data) response.data = malloc(TIMEZONE_RESPONSE_LIMIT);
    if (!response.data) {
        strlcpy(result->message, "Not enough memory", sizeof(result->message));
        return false;
    }
    response.data[0] = '\0';
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "SpacePC-Orb/0.2 (+https://spacepc.dev; contact: hello@spacepc.dev)",
    };
    if (!orb_net_take(pdMS_TO_TICKS(30000))) {
        free(response.data);
        strlcpy(result->message, "Timezone service busy", sizeof(result->message));
        *retryable = true;
        return false;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        orb_net_give();
        free(response.data);
        strlcpy(result->message, "Timezone service unavailable", sizeof(result->message));
        return false;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent",
                               "SpacePC-Orb/0.2 (+https://spacepc.dev; contact: hello@spacepc.dev)");
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    orb_net_give();
    if (err != ESP_OK || status != 200 || response.failed) {
        ESP_LOGW(TAG, "Timezone lookup failed: %s, HTTP %d", esp_err_to_name(err), status);
        free(response.data);
        strlcpy(result->message, "Timezone lookup failed", sizeof(result->message));
        *retryable = err != ESP_OK || status == 0 || status == 429 || status >= 500;
        return false;
    }

    cJSON *root = cJSON_Parse(response.data);
    free(response.data);
    cJSON *zone = root ? cJSON_GetObjectItemCaseSensitive(root, "timeZone") : NULL;
    cJSON *offset = root ? cJSON_GetObjectItemCaseSensitive(root, "currentUtcOffset") : NULL;
    cJSON *seconds = offset ? cJSON_GetObjectItemCaseSensitive(offset, "seconds") : NULL;
    if (!cJSON_IsString(zone) || !cJSON_IsNumber(seconds) || seconds->valueint < -43200 ||
        seconds->valueint > 50400) {
        cJSON_Delete(root);
        strlcpy(result->message, "Timezone not found", sizeof(result->message));
        return false;
    }
    strlcpy(result->timezone, zone->valuestring, sizeof(result->timezone));
    result->utc_offset_seconds = seconds->valueint;
    result->success = true;
    strlcpy(result->message, "Timezone synchronized", sizeof(result->message));
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%s, UTC offset %+ld seconds", result->timezone,
             (long)result->utc_offset_seconds);
    return true;
}

static void timezone_task(void *argument)
{
    (void)argument;
    timezone_request_t request;
    while (xQueueReceive(s_requests, &request, portMAX_DELAY) == pdTRUE) {
        ESP_LOGI(TAG, "Resolving timezone for %.5f, %.5f", request.latitude, request.longitude);
        orb_timezone_result_t result = {
            .latitude = request.latitude,
            .longitude = request.longitude,
        };
        bool connected = false;
        for (int attempt = 0; attempt < 120; attempt++) {
            orb_wifi_status_t wifi;
            orb_wifi_get_status(&wifi);
            if (wifi.connected) {
                connected = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (!connected) {
            strlcpy(result.message, "Connect WiFi for local time", sizeof(result.message));
        } else orb_timezone_lookup_now(request.latitude, request.longitude, &result);
        xQueueOverwrite(s_results, &result);
    }
}

esp_err_t orb_timezone_init(void)
{
    if (s_requests) return ESP_OK;
    s_requests = xQueueCreate(1, sizeof(timezone_request_t));
    s_results = xQueueCreate(1, sizeof(orb_timezone_result_t));
    if (!s_requests || !s_results) return ESP_ERR_NO_MEM;
    if (xTaskCreate(timezone_task, "orb_timezone", 10240, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_requests);
        vQueueDelete(s_results);
        s_requests = NULL;
        s_results = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool orb_timezone_request(double latitude, double longitude)
{
    if (!s_requests || latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) {
        return false;
    }
    const timezone_request_t request = {.latitude = latitude, .longitude = longitude};
    return xQueueOverwrite(s_requests, &request) == pdTRUE;
}

bool orb_timezone_take_result(orb_timezone_result_t *result)
{
    return result && s_results && xQueueReceive(s_results, result, 0) == pdTRUE;
}

bool orb_timezone_lookup_now(double latitude, double longitude, orb_timezone_result_t *result)
{
    if (!result || latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    result->latitude = latitude;
    result->longitude = longitude;
    const timezone_request_t request = {.latitude = latitude, .longitude = longitude};
    for (int attempt = 1; attempt <= 4; attempt++) {
        bool retryable = false;
        if (lookup_timezone(&request, result, &retryable)) return true;
        if (!retryable) return false;
        if (attempt < 4) vTaskDelay(pdMS_TO_TICKS(2500));
    }
    return false;
}
