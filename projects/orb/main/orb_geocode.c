#include "orb_geocode.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "orb_net.h"
#include "orb_timezone.h"
#include "orb_wifi.h"

#define GEOCODE_RESPONSE_LIMIT (16 * 1024)

typedef struct {
    char city[64];
} geocode_request_t;

typedef struct {
    char *data;
    size_t size;
    bool failed;
} response_buffer_t;

static const char *TAG = "orb_geocode";
static QueueHandle_t s_requests;
static QueueHandle_t s_results;

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_buffer_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !response || response->failed) return ESP_OK;
    if (response->size + event->data_len + 1 > GEOCODE_RESPONSE_LIMIT) {
        response->failed = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(response->data + response->size, event->data, event->data_len);
    response->size += event->data_len;
    response->data[response->size] = '\0';
    return ESP_OK;
}

static void url_encode(const char *input, char *output, size_t output_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    while (*input && used + 1 < output_size) {
        const unsigned char value = (unsigned char)*input++;
        if (isalnum(value) || value == '-' || value == '_' || value == '.' || value == '~') {
            output[used++] = (char)value;
        } else if (used + 3 < output_size) {
            output[used++] = '%';
            output[used++] = hex[value >> 4];
            output[used++] = hex[value & 0x0f];
        } else {
            break;
        }
    }
    output[used] = '\0';
}

static bool lookup_city(const char *city, orb_geocode_result_t *result, bool *retryable)
{
    *retryable = false;
    char encoded[192];
    char url[320];
    url_encode(city, encoded, sizeof(encoded));
    snprintf(url, sizeof(url),
             "https://nominatim.openstreetmap.org/search?q=%s&format=jsonv2&limit=1&addressdetails=0",
             encoded);

    response_buffer_t response = {
        .data = heap_caps_malloc(GEOCODE_RESPONSE_LIMIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    if (!response.data) response.data = malloc(GEOCODE_RESPONSE_LIMIT);
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
        strlcpy(result->message, "Location service busy", sizeof(result->message));
        *retryable = true;
        return false;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        orb_net_give();
        free(response.data);
        strlcpy(result->message, "Location service unavailable", sizeof(result->message));
        return false;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Language", "en");
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    orb_net_give();
    if (err != ESP_OK || status != 200 || response.failed) {
        ESP_LOGW(TAG, "City lookup failed: %s, HTTP %d", esp_err_to_name(err), status);
        free(response.data);
        strlcpy(result->message, "City lookup failed", sizeof(result->message));
        *retryable = err != ESP_OK || status == 0 || status == 429 || status >= 500;
        return false;
    }

    cJSON *root = cJSON_Parse(response.data);
    free(response.data);
    cJSON *place = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : NULL;
    cJSON *latitude = place ? cJSON_GetObjectItemCaseSensitive(place, "lat") : NULL;
    cJSON *longitude = place ? cJSON_GetObjectItemCaseSensitive(place, "lon") : NULL;
    if (!cJSON_IsString(latitude) || !cJSON_IsString(longitude)) {
        cJSON_Delete(root);
        strlcpy(result->message, "City not found", sizeof(result->message));
        return false;
    }
    result->latitude = strtod(latitude->valuestring, NULL);
    result->longitude = strtod(longitude->valuestring, NULL);
    cJSON_Delete(root);
    result->success = true;
    strlcpy(result->message, "Location saved", sizeof(result->message));
    return true;
}

static void geocode_task(void *argument)
{
    (void)argument;
    int64_t last_request_us = 0;
    geocode_request_t request;
    while (xQueueReceive(s_requests, &request, portMAX_DELAY) == pdTRUE) {
        orb_geocode_result_t result = {0};
        strlcpy(result.city, request.city, sizeof(result.city));

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
            strlcpy(result.message, "Connect WiFi to find the city", sizeof(result.message));
        } else {
            const int64_t elapsed = esp_timer_get_time() - last_request_us;
            if (elapsed < 1000000) vTaskDelay(pdMS_TO_TICKS((1000000 - elapsed) / 1000));
            for (int attempt = 1; attempt <= 4; attempt++) {
                bool retryable = false;
                if (lookup_city(request.city, &result, &retryable) || !retryable) break;
                if (attempt < 4) {
                    ESP_LOGW(TAG, "Retrying city lookup after network/DNS startup (%d/4)", attempt + 1);
                    vTaskDelay(pdMS_TO_TICKS(2500));
                }
            }
            if (result.success) {
                orb_timezone_result_t timezone;
                if (orb_timezone_lookup_now(result.latitude, result.longitude, &timezone)) {
                    strlcpy(result.timezone, timezone.timezone, sizeof(result.timezone));
                    result.utc_offset_seconds = timezone.utc_offset_seconds;
                }
            }
            last_request_us = esp_timer_get_time();
        }
        xQueueOverwrite(s_results, &result);
    }
}

esp_err_t orb_geocode_init(void)
{
    if (s_requests) return ESP_OK;
    s_requests = xQueueCreate(1, sizeof(geocode_request_t));
    s_results = xQueueCreate(1, sizeof(orb_geocode_result_t));
    if (!s_requests || !s_results) return ESP_ERR_NO_MEM;
    if (xTaskCreate(geocode_task, "orb_geocode", 12288, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool orb_geocode_request(const char *city)
{
    if (!s_requests || !city || !city[0]) return false;
    geocode_request_t request = {0};
    strlcpy(request.city, city, sizeof(request.city));
    return xQueueOverwrite(s_requests, &request) == pdTRUE;
}

bool orb_geocode_take_result(orb_geocode_result_t *result)
{
    return result && s_results && xQueueReceive(s_results, result, 0) == pdTRUE;
}
