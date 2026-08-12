#include "orb_route.h"

#include <math.h>
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ROUTE_RESPONSE_LIMIT (24 * 1024)
#define ROUTE_CACHE_SIZE 8

typedef struct {
    char callsign[12];
    double latitude;
    double longitude;
} route_request_t;

typedef enum {
    ROUTE_LOOKUP_ERROR,
    ROUTE_LOOKUP_NOT_FOUND,
    ROUTE_LOOKUP_FOUND,
} route_lookup_state_t;

typedef struct {
    char *data;
    size_t size;
    bool failed;
} response_buffer_t;

static const char *TAG = "orb_route";
static QueueHandle_t s_requests;
static QueueHandle_t s_results;
static orb_route_result_t s_cache[ROUTE_CACHE_SIZE];
static size_t s_cache_next;

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_buffer_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !response || response->failed) return ESP_OK;
    if (response->size + event->data_len + 1 > ROUTE_RESPONSE_LIMIT) {
        response->failed = true;
        return ESP_ERR_NO_MEM;
    }
    memcpy(response->data + response->size, event->data, event->data_len);
    response->size += event->data_len;
    response->data[response->size] = '\0';
    return ESP_OK;
}

static const char *json_string(cJSON *object, const char *name)
{
    cJSON *value = object ? cJSON_GetObjectItemCaseSensitive(object, name) : NULL;
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static double json_number(cJSON *object, const char *name)
{
    cJSON *value = object ? cJSON_GetObjectItemCaseSensitive(object, name) : NULL;
    return cJSON_IsNumber(value) ? value->valuedouble : NAN;
}

static uint16_t estimate_duration_coords(double origin_latitude, double origin_longitude,
                                         double destination_latitude, double destination_longitude)
{
    const double lat1 = origin_latitude * M_PI / 180.0;
    const double lon1 = origin_longitude * M_PI / 180.0;
    const double lat2 = destination_latitude * M_PI / 180.0;
    const double lon2 = destination_longitude * M_PI / 180.0;
    if (!isfinite(lat1) || !isfinite(lon1) || !isfinite(lat2) || !isfinite(lon2)) return 0;
    const double dlat = lat2 - lat1;
    const double dlon = lon2 - lon1;
    const double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat1) * cos(lat2) * sin(dlon / 2) * sin(dlon / 2);
    const double distance_nm = 3440.065 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    const double minutes = distance_nm / 430.0 * 60.0 + 20.0;
    return (uint16_t)lround(fmin(1440.0, fmax(20.0, minutes)));
}

static bool fetch_json(const char *url, char **body, int *http_status)
{
    *body = NULL;
    *http_status = 0;
    response_buffer_t response = {
        .data = heap_caps_malloc(ROUTE_RESPONSE_LIMIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    if (!response.data) response.data = malloc(ROUTE_RESPONSE_LIMIT);
    if (!response.data) return false;
    response.data[0] = '\0';
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 12000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "SpacePC-Orb/0.2 (+https://spacepc.dev; contact: hello@spacepc.dev)",
    };
    if (!orb_net_take(pdMS_TO_TICKS(30000))) {
        free(response.data);
        return false;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        orb_net_give();
        free(response.data);
        return false;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    *http_status = status;
    esp_http_client_cleanup(client);
    orb_net_give();
    if (err != ESP_OK || status != 200 || response.failed) {
        ESP_LOGW(TAG, "GET %s failed: %s, HTTP %d", url, esp_err_to_name(err), status);
        free(response.data);
        return false;
    }
    *body = response.data;
    return true;
}

static route_lookup_state_t lookup_adsbdb(const char *callsign, orb_route_result_t *result)
{
    char url[160];
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", callsign);
    char *body = NULL;
    int status = 0;
    if (!fetch_json(url, &body, &status)) return status == 404 ? ROUTE_LOOKUP_NOT_FOUND : ROUTE_LOOKUP_ERROR;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return ROUTE_LOOKUP_ERROR;
    cJSON *response_object = root ? cJSON_GetObjectItemCaseSensitive(root, "response") : NULL;
    cJSON *route = response_object ? cJSON_GetObjectItemCaseSensitive(response_object, "flightroute") : NULL;
    cJSON *origin = route ? cJSON_GetObjectItemCaseSensitive(route, "origin") : NULL;
    cJSON *destination = route ? cJSON_GetObjectItemCaseSensitive(route, "destination") : NULL;
    if (!cJSON_IsObject(origin) || !cJSON_IsObject(destination)) {
        cJSON_Delete(root);
        return ROUTE_LOOKUP_NOT_FOUND;
    }
    const char *origin_iata = json_string(origin, "iata_code");
    const char *origin_icao = json_string(origin, "icao_code");
    const char *destination_iata = json_string(destination, "iata_code");
    const char *destination_icao = json_string(destination, "icao_code");
    strlcpy(result->origin_code, origin_iata && origin_iata[0] ? origin_iata : (origin_icao ?: "---"), sizeof(result->origin_code));
    strlcpy(result->destination_code, destination_iata && destination_iata[0] ? destination_iata : (destination_icao ?: "---"), sizeof(result->destination_code));
    strlcpy(result->origin_city, json_string(origin, "municipality") ?: "Unknown", sizeof(result->origin_city));
    strlcpy(result->destination_city, json_string(destination, "municipality") ?: "Unknown", sizeof(result->destination_city));
    result->estimated_minutes = estimate_duration_coords(
        json_number(origin, "latitude"), json_number(origin, "longitude"),
        json_number(destination, "latitude"), json_number(destination, "longitude"));
    result->success = true;
    strlcpy(result->message, "Route ready", sizeof(result->message));
    cJSON_Delete(root);
    return ROUTE_LOOKUP_FOUND;
}

static route_lookup_state_t lookup_adsblol(const char *callsign, double latitude, double longitude,
                                           orb_route_result_t *result)
{
    char url[192];
    snprintf(url, sizeof(url), "https://api.adsb.lol/api/0/route/%s/%.5f/%.5f",
             callsign, latitude, longitude);
    char *body = NULL;
    int status = 0;
    if (!fetch_json(url, &body, &status)) return status == 404 ? ROUTE_LOOKUP_NOT_FOUND : ROUTE_LOOKUP_ERROR;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return ROUTE_LOOKUP_ERROR;
    const char *airport_codes = json_string(root, "airport_codes");
    cJSON *plausible = cJSON_GetObjectItemCaseSensitive(root, "plausible");
    cJSON *airports = cJSON_GetObjectItemCaseSensitive(root, "_airports");
    const int airport_count = cJSON_IsArray(airports) ? cJSON_GetArraySize(airports) : 0;
    if (!airport_codes || strcmp(airport_codes, "unknown") == 0 || cJSON_IsFalse(plausible) ||
        airport_count < 2) {
        cJSON_Delete(root);
        return ROUTE_LOOKUP_NOT_FOUND;
    }
    cJSON *origin = cJSON_GetArrayItem(airports, 0);
    cJSON *destination = cJSON_GetArrayItem(airports, airport_count - 1);
    if (!cJSON_IsObject(origin) || !cJSON_IsObject(destination)) {
        cJSON_Delete(root);
        return ROUTE_LOOKUP_NOT_FOUND;
    }
    const char *origin_iata = json_string(origin, "iata");
    const char *origin_icao = json_string(origin, "icao");
    const char *destination_iata = json_string(destination, "iata");
    const char *destination_icao = json_string(destination, "icao");
    strlcpy(result->origin_code, origin_iata && origin_iata[0] ? origin_iata : (origin_icao ?: "---"),
            sizeof(result->origin_code));
    strlcpy(result->destination_code,
            destination_iata && destination_iata[0] ? destination_iata : (destination_icao ?: "---"),
            sizeof(result->destination_code));
    strlcpy(result->origin_city, json_string(origin, "location") ?: "Unknown", sizeof(result->origin_city));
    strlcpy(result->destination_city, json_string(destination, "location") ?: "Unknown",
            sizeof(result->destination_city));
    result->estimated_minutes = estimate_duration_coords(
        json_number(origin, "lat"), json_number(origin, "lon"),
        json_number(destination, "lat"), json_number(destination, "lon"));
    result->success = true;
    strlcpy(result->message, "Route ready", sizeof(result->message));
    cJSON_Delete(root);
    return ROUTE_LOOKUP_FOUND;
}

static route_lookup_state_t lookup_route(const route_request_t *request, orb_route_result_t *result)
{
    const route_lookup_state_t adsbdb = lookup_adsbdb(request->callsign, result);
    if (adsbdb == ROUTE_LOOKUP_FOUND) return adsbdb;
    const route_lookup_state_t adsblol = lookup_adsblol(request->callsign, request->latitude,
                                                       request->longitude, result);
    if (adsblol == ROUTE_LOOKUP_FOUND) return adsblol;
    return adsbdb == ROUTE_LOOKUP_NOT_FOUND && adsblol == ROUTE_LOOKUP_NOT_FOUND
               ? ROUTE_LOOKUP_NOT_FOUND
               : ROUTE_LOOKUP_ERROR;
}

static void route_task(void *argument)
{
    (void)argument;
    route_request_t request;
    while (xQueueReceive(s_requests, &request, portMAX_DELAY) == pdTRUE) {
        orb_route_result_t result = {0};
        strlcpy(result.callsign, request.callsign, sizeof(result.callsign));
        orb_wifi_status_t wifi;
        orb_wifi_get_status(&wifi);
        route_lookup_state_t state = ROUTE_LOOKUP_ERROR;
        if (wifi.connected) {
            state = lookup_route(&request, &result);
            if (state == ROUTE_LOOKUP_ERROR) {
                vTaskDelay(pdMS_TO_TICKS(900));
                memset(&result, 0, sizeof(result));
                strlcpy(result.callsign, request.callsign, sizeof(result.callsign));
                state = lookup_route(&request, &result);
            }
        }
        if (!wifi.connected) strlcpy(result.message, "Route needs WiFi", sizeof(result.message));
        else if (state == ROUTE_LOOKUP_NOT_FOUND) {
            strlcpy(result.message, "No scheduled route", sizeof(result.message));
        } else if (state == ROUTE_LOOKUP_ERROR) {
            strlcpy(result.message, "Route service unavailable", sizeof(result.message));
        }
        if (result.success) s_cache[s_cache_next++ % ROUTE_CACHE_SIZE] = result;
        xQueueOverwrite(s_results, &result);
    }
}

esp_err_t orb_route_init(void)
{
    if (s_requests) return ESP_OK;
    s_requests = xQueueCreate(1, sizeof(route_request_t));
    s_results = xQueueCreate(1, sizeof(orb_route_result_t));
    if (!s_requests || !s_results) return ESP_ERR_NO_MEM;
    if (xTaskCreate(route_task, "orb_route", 12288, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool orb_route_request(const char *callsign, double latitude, double longitude)
{
    if (!s_requests || !callsign || !callsign[0] || strcmp(callsign, "UNKNOWN") == 0) return false;
    for (size_t i = 0; i < ROUTE_CACHE_SIZE; i++) {
        if (strcmp(s_cache[i].callsign, callsign) == 0) return xQueueOverwrite(s_results, &s_cache[i]) == pdTRUE;
    }
    route_request_t request = {0};
    strlcpy(request.callsign, callsign, sizeof(request.callsign));
    request.latitude = latitude;
    request.longitude = longitude;
    return xQueueOverwrite(s_requests, &request) == pdTRUE;
}

bool orb_route_take_result(orb_route_result_t *result)
{
    return result && s_results && xQueueReceive(s_results, result, 0) == pdTRUE;
}
