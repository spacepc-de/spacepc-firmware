#include "orb_data.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "orb_net.h"
#include "orb_wifi.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "orb_data";
static SemaphoreHandle_t s_lock;
static orb_data_snapshot_t s_data;
static double s_observer_lat;
static double s_observer_lon;
static uint16_t s_radius_nm;
static TaskHandle_t s_task;
static volatile bool s_refresh_requested;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
    size_t limit;
    bool failed;
} response_buffer_t;

static double radians(double value) { return value * M_PI / 180.0; }
static double degrees(double value) { return value * 180.0 / M_PI; }

static void trim(char *text)
{
    size_t length = strlen(text);
    while (length && text[length - 1] == ' ') text[--length] = '\0';
}

static void relative_position(double lat, double lon, float *distance_nm, float *bearing_deg)
{
    const double lat1 = radians(s_observer_lat);
    const double lat2 = radians(lat);
    const double dlat = lat2 - lat1;
    const double dlon = radians(lon - s_observer_lon);
    const double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat1) * cos(lat2) * sin(dlon / 2) * sin(dlon / 2);
    const double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    const double y = sin(dlon) * cos(lat2);
    const double x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dlon);
    *distance_nm = (float)(3440.065 * c);
    *bearing_deg = (float)fmod(degrees(atan2(y, x)) + 360.0, 360.0);
}

static void destination(double lat, double lon, double bearing, double distance_nm,
                        double *out_lat, double *out_lon)
{
    const double angular = distance_nm / 3440.065;
    const double phi1 = radians(lat);
    const double lambda1 = radians(lon);
    const double theta = radians(bearing);
    const double phi2 = asin(sin(phi1) * cos(angular) + cos(phi1) * sin(angular) * cos(theta));
    const double lambda2 = lambda1 + atan2(sin(theta) * sin(angular) * cos(phi1),
                                            cos(angular) - sin(phi1) * sin(phi2));
    *out_lat = degrees(phi2);
    *out_lon = fmod(degrees(lambda2) + 540.0, 360.0) - 180.0;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_buffer_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !response || response->failed) return ESP_OK;
    if (response->size + event->data_len + 1 > response->limit) {
        response->failed = true;
        return ESP_ERR_NO_MEM;
    }
    if (response->size + event->data_len + 1 > response->capacity) {
        size_t capacity = response->capacity ? response->capacity * 2 : 4096;
        while (capacity < response->size + event->data_len + 1) capacity *= 2;
        if (capacity > response->limit) capacity = response->limit;
        char *grown = heap_caps_realloc(response->data, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!grown) grown = realloc(response->data, capacity);
        if (!grown) {
            response->failed = true;
            return ESP_ERR_NO_MEM;
        }
        response->data = grown;
        response->capacity = capacity;
    }
    memcpy(response->data + response->size, event->data, event->data_len);
    response->size += event->data_len;
    response->data[response->size] = '\0';
    return ESP_OK;
}

static char *http_get(const char *url, size_t limit)
{
    response_buffer_t response = {.limit = limit};
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 12000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .user_agent = "SpacePC-Orb/0.1 (+https://spacepc.dev)",
        .keep_alive_enable = true,
    };
    if (!orb_net_take(pdMS_TO_TICKS(30000))) return NULL;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        orb_net_give();
        return NULL;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    orb_net_give();
    if (err != ESP_OK || status < 200 || status >= 300 || response.failed || !response.data) {
        ESP_LOGW(TAG, "GET %s failed: %s, HTTP %d", url, esp_err_to_name(err), status);
        free(response.data);
        return NULL;
    }
    return response.data;
}

static const char *json_string(cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static double json_number(cJSON *object, const char *name, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static bool update_aircraft(void)
{
    char url[160];
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.5f/%.5f/%u",
             s_observer_lat, s_observer_lon, s_radius_nm);
    char *body = http_get(url, 160 * 1024);
    if (!body) return false;
    cJSON *root = cJSON_Parse(body);
    free(body);
    cJSON *array = root ? cJSON_GetObjectItemCaseSensitive(root, "ac") : NULL;
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(root);
        return false;
    }
    orb_aircraft_t aircraft[ORB_MAX_AIRCRAFT] = {0};
    size_t count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, array) {
        const double lat = json_number(item, "lat", NAN);
        const double lon = json_number(item, "lon", NAN);
        if (!isfinite(lat) || !isfinite(lon) || count == ORB_MAX_AIRCRAFT) continue;
        orb_aircraft_t *plane = &aircraft[count];
        const char *callsign = json_string(item, "flight");
        const char *registration = json_string(item, "r");
        const char *type = json_string(item, "t");
        strlcpy(plane->callsign, callsign ? callsign : (registration ? registration : "UNKNOWN"), sizeof(plane->callsign));
        strlcpy(plane->registration, registration ? registration : "--", sizeof(plane->registration));
        strlcpy(plane->type, type ? type : "--", sizeof(plane->type));
        trim(plane->callsign);
        plane->latitude = lat;
        plane->longitude = lon;
        plane->speed_knots = (float)json_number(item, "gs", 0);
        plane->track_deg = (float)json_number(item, "track", 0);
        cJSON *altitude = cJSON_GetObjectItemCaseSensitive(item, "alt_baro");
        plane->on_ground = cJSON_IsString(altitude) && strcmp(altitude->valuestring, "ground") == 0;
        plane->altitude_ft = cJSON_IsNumber(altitude) ? (float)altitude->valuedouble : 0;
        relative_position(lat, lon, &plane->distance_nm, &plane->bearing_deg);
        count++;
    }
    cJSON_Delete(root);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_data.aircraft, aircraft, sizeof(aircraft));
    s_data.aircraft_count = count;
    s_data.aircraft_live = true;
    s_data.aircraft_updated_unix = time(NULL);
    s_data.last_error[0] = '\0';
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Received %u nearby aircraft", (unsigned)count);
    return true;
}

static bool update_iss(void)
{
    char *body = http_get("https://api.wheretheiss.at/v1/satellites/25544", 8192);
    if (!body) return false;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return false;
    const double lat = json_number(root, "latitude", NAN);
    const double lon = json_number(root, "longitude", NAN);
    if (!isfinite(lat) || !isfinite(lon)) {
        cJSON_Delete(root);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data.iss_latitude = lat;
    s_data.iss_longitude = lon;
    s_data.iss_altitude_km = (float)json_number(root, "altitude", 0);
    s_data.iss_velocity_kmh = (float)json_number(root, "velocity", 0);
    strlcpy(s_data.iss_visibility, json_string(root, "visibility") ?: "unknown", sizeof(s_data.iss_visibility));
    s_data.iss_updated_unix = (uint64_t)json_number(root, "timestamp", time(NULL));
    s_data.iss_live = true;
    s_data.last_error[0] = '\0';
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);
    return true;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static double parse_epoch(const char *text)
{
    int year, month, day, hour, minute;
    double second;
    if (!text || sscanf(text, "%d-%d-%dT%d:%d:%lf", &year, &month, &day, &hour, &minute, &second) != 6) return 0;
    return days_from_civil(year, month, day) * 86400.0 + hour * 3600.0 + minute * 60.0 + second;
}

static bool useful_satellite_name(const char *name)
{
    if (!name || strstr(name, " DEB") || strstr(name, "R/B") || strstr(name, "SL-") || strstr(name, "ATLAS")) return false;
    return true;
}

static bool update_satellites(void)
{
    char *body = http_get("https://celestrak.org/NORAD/elements/gp.php?GROUP=VISUAL&FORMAT=JSON", 256 * 1024);
    if (!body) return false;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }
    orb_satellite_t satellites[ORB_MAX_SATELLITES] = {0};
    size_t count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        const char *name = json_string(item, "OBJECT_NAME");
        if (!useful_satellite_name(name) || count == ORB_MAX_SATELLITES) continue;
        orb_satellite_t *sat = &satellites[count++];
        strlcpy(sat->name, name, sizeof(sat->name));
        sat->catalog_id = (uint32_t)json_number(item, "NORAD_CAT_ID", 0);
        sat->epoch_unix = parse_epoch(json_string(item, "EPOCH"));
        sat->mean_motion = json_number(item, "MEAN_MOTION", 0);
        sat->eccentricity = json_number(item, "ECCENTRICITY", 0);
        sat->inclination = json_number(item, "INCLINATION", 0);
        sat->raan = json_number(item, "RA_OF_ASC_NODE", 0);
        sat->argument_perigee = json_number(item, "ARG_OF_PERICENTER", 0);
        sat->mean_anomaly = json_number(item, "MEAN_ANOMALY", 0);
    }
    cJSON_Delete(root);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_data.satellites, satellites, sizeof(satellites));
    s_data.satellite_count = count;
    s_data.satellites_live = count > 0;
    s_data.catalog_updated_unix = time(NULL);
    s_data.last_error[0] = '\0';
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Loaded %u visual satellite element sets", (unsigned)count);
    return count > 0;
}

static void propagate_satellite(orb_satellite_t *satellite, double unix_seconds)
{
    if (satellite->mean_motion <= 0 || satellite->epoch_unix <= 0) return;
    const double n = satellite->mean_motion * 2.0 * M_PI / 86400.0;
    double mean = radians(satellite->mean_anomaly) + n * (unix_seconds - satellite->epoch_unix);
    mean = fmod(mean, 2.0 * M_PI);
    double eccentric = mean;
    for (int i = 0; i < 7; i++) eccentric = mean + satellite->eccentricity * sin(eccentric);
    const double true_anomaly = 2.0 * atan2(sqrt(1 + satellite->eccentricity) * sin(eccentric / 2),
                                            sqrt(1 - satellite->eccentricity) * cos(eccentric / 2));
    const double u = radians(satellite->argument_perigee) + true_anomaly;
    const double ascending = radians(satellite->raan);
    const double inclination = radians(satellite->inclination);
    const double x = cos(ascending) * cos(u) - sin(ascending) * sin(u) * cos(inclination);
    const double y = sin(ascending) * cos(u) + cos(ascending) * sin(u) * cos(inclination);
    const double z = sin(u) * sin(inclination);
    const double jd = unix_seconds / 86400.0 + 2440587.5;
    const double theta = radians(fmod(280.46061837 + 360.98564736629 * (jd - 2451545.0), 360.0));
    const double ecef_x = cos(theta) * x + sin(theta) * y;
    const double ecef_y = -sin(theta) * x + cos(theta) * y;
    satellite->latitude = degrees(atan2(z, sqrt(ecef_x * ecef_x + ecef_y * ecef_y)));
    satellite->longitude = degrees(atan2(ecef_y, ecef_x));
}

static void seed_demo_data(void)
{
    static const struct { const char *call; const char *reg; const char *type; float distance; float bearing; float track; float altitude; float speed; } demo[] = {
        {"ORB 217", "D-AIXP", "A350", 18, 32, 154, 12600, 322},
        {"DLH 4KT", "D-AIZQ", "A320", 42, 105, 278, 34800, 451},
        {"RYR 82M", "EI-IGJ", "B38M", 63, 231, 42, 37100, 438},
        {"EJU 51P", "OE-IZQ", "A320", 29, 298, 116, 8200, 264},
        {"OE-KLX", "OE-KLX", "C172", 12, 182, 351, 4800, 101},
        {"SXS 7AJ", "TC-SPE", "B738", 72, 350, 193, 36000, 462},
    };
    for (size_t i = 0; i < sizeof(demo) / sizeof(demo[0]); i++) {
        orb_aircraft_t *plane = &s_data.aircraft[i];
        strlcpy(plane->callsign, demo[i].call, sizeof(plane->callsign));
        strlcpy(plane->registration, demo[i].reg, sizeof(plane->registration));
        strlcpy(plane->type, demo[i].type, sizeof(plane->type));
        plane->distance_nm = demo[i].distance;
        plane->bearing_deg = demo[i].bearing;
        plane->track_deg = demo[i].track;
        plane->altitude_ft = demo[i].altitude;
        plane->speed_knots = demo[i].speed;
        destination(s_observer_lat, s_observer_lon, demo[i].bearing, demo[i].distance, &plane->latitude, &plane->longitude);
    }
    s_data.aircraft_count = sizeof(demo) / sizeof(demo[0]);
    static const struct { const char *name; uint32_t id; double mm, inc, raan, ma; } sats[] = {
        {"ISS (ZARYA)", 25544, 15.49, 51.64, 73, 18}, {"HUBBLE", 20580, 15.09, 28.47, 192, 220},
        {"TIANGONG", 48274, 15.61, 41.47, 310, 94}, {"NOAA 19", 33591, 14.12, 99.19, 124, 310},
        {"STARLINK", 44713, 15.05, 53.05, 42, 165}, {"TERRA", 25994, 14.57, 98.21, 251, 42},
    };
    const double now = 1700000000.0;
    for (size_t i = 0; i < sizeof(sats) / sizeof(sats[0]); i++) {
        orb_satellite_t *sat = &s_data.satellites[i];
        strlcpy(sat->name, sats[i].name, sizeof(sat->name));
        sat->catalog_id = sats[i].id;
        sat->epoch_unix = now;
        sat->mean_motion = sats[i].mm;
        sat->inclination = sats[i].inc;
        sat->raan = sats[i].raan;
        sat->mean_anomaly = sats[i].ma;
        sat->argument_perigee = 80 + i * 17;
        sat->eccentricity = 0.001;
    }
    s_data.satellite_count = sizeof(sats) / sizeof(sats[0]);
    s_data.iss_latitude = 18;
    s_data.iss_longitude = -34;
    s_data.iss_altitude_km = 419;
    s_data.iss_velocity_kmh = 27600;
    strlcpy(s_data.iss_visibility, "daylight", sizeof(s_data.iss_visibility));
}

static void data_task(void *argument)
{
    (void)argument;
    TickType_t last_aircraft = 0, last_iss = 0, last_catalog = 0;
    while (true) {
        orb_wifi_status_t wifi;
        orb_wifi_get_status(&wifi);
        const TickType_t now = xTaskGetTickCount();
        if (wifi.connected) {
            bool did_work = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_data.refreshing = true;
            xSemaphoreGive(s_lock);
            if (s_refresh_requested || !last_aircraft || now - last_aircraft >= pdMS_TO_TICKS(15000)) {
                if (!update_aircraft()) {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    strlcpy(s_data.last_error, "Aircraft feed unavailable", sizeof(s_data.last_error));
                    xSemaphoreGive(s_lock);
                }
                last_aircraft = now;
                did_work = true;
                vTaskDelay(pdMS_TO_TICKS(350));
            }
            if (s_refresh_requested || !last_iss || now - last_iss >= pdMS_TO_TICKS(10000)) {
                update_iss();
                last_iss = now;
                did_work = true;
                vTaskDelay(pdMS_TO_TICKS(350));
            }
            if (s_refresh_requested || !last_catalog || now - last_catalog >= pdMS_TO_TICKS(6 * 60 * 60 * 1000)) {
                update_satellites();
                last_catalog = now;
                did_work = true;
            }
            s_refresh_requested = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_data.refreshing = false;
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(did_work ? 1000 : 2000));
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_data.refreshing = false;
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

esp_err_t orb_data_init(const orb_settings_t *settings)
{
    if (!settings) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(orb_net_init(), TAG, "network lock");
    s_observer_lat = settings->latitude;
    s_observer_lon = settings->longitude;
    s_radius_nm = settings->aircraft_radius_nm;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    seed_demo_data();
    if (xTaskCreate(data_task, "orb_live_data", 16384, NULL, 4, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void orb_data_set_observer(double latitude, double longitude, uint16_t radius_nm)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_observer_lat = latitude;
    s_observer_lon = longitude;
    s_radius_nm = radius_nm;
    xSemaphoreGive(s_lock);
    s_refresh_requested = true;
}

void orb_data_refresh_now(void)
{
    s_refresh_requested = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void orb_data_get_snapshot(orb_data_snapshot_t *snapshot)
{
    if (!snapshot || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *snapshot = s_data;
    xSemaphoreGive(s_lock);

    const double uptime_s = esp_timer_get_time() / 1000000.0;
    const double current_unix = time(NULL) >= 1609459200 ? (double)time(NULL) : 1700000000.0 + uptime_s;
    for (size_t i = 0; i < snapshot->satellite_count; i++) propagate_satellite(&snapshot->satellites[i], current_unix);

    if (!snapshot->aircraft_live) {
        for (size_t i = 0; i < snapshot->aircraft_count; i++) {
            orb_aircraft_t *plane = &snapshot->aircraft[i];
            const double travel_nm = plane->speed_knots * fmod(uptime_s, 120.0) / 3600.0;
            destination(plane->latitude, plane->longitude, plane->track_deg, travel_nm, &plane->latitude, &plane->longitude);
            relative_position(plane->latitude, plane->longitude, &plane->distance_nm, &plane->bearing_deg);
        }
        snapshot->iss_latitude = 51.6 * sin(uptime_s / 900.0);
        snapshot->iss_longitude = fmod(-120.0 + uptime_s * 0.065 + 540.0, 360.0) - 180.0;
    } else {
        const double age = fmin(20.0, fmax(0.0, current_unix - snapshot->aircraft_updated_unix));
        for (size_t i = 0; i < snapshot->aircraft_count; i++) {
            orb_aircraft_t *plane = &snapshot->aircraft[i];
            destination(plane->latitude, plane->longitude, plane->track_deg,
                        plane->speed_knots * age / 3600.0, &plane->latitude, &plane->longitude);
            relative_position(plane->latitude, plane->longitude, &plane->distance_nm, &plane->bearing_deg);
        }
    }
}
