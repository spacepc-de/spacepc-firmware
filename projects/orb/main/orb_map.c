#include "orb_map.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "orb_net.h"
#include "orb_wifi.h"
#include "png.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAP_CACHE_ROOT BSP_SD_MOUNT_POINT "/ORB/MAPS"
#define MAP_TILE_LIMIT (256 * 1024)

typedef struct {
    uint32_t generation;
    double latitude;
    double longitude;
    uint8_t zoom;
} map_request_t;

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
} response_buffer_t;

static const char *TAG = "orb_map";
static QueueHandle_t s_request_queue;
static QueueHandle_t s_result_queue;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static orb_map_status_t s_status;
static uint32_t s_generation;

static double clamp_latitude(double latitude)
{
    if (latitude > 85.05112878) return 85.05112878;
    if (latitude < -85.05112878) return -85.05112878;
    return latitude;
}

static double longitude_to_tile_x(double longitude, uint8_t zoom)
{
    return (longitude + 180.0) / 360.0 * (1u << zoom);
}

static double latitude_to_tile_y(double latitude, uint8_t zoom)
{
    const double radians = clamp_latitude(latitude) * M_PI / 180.0;
    return (1.0 - asinh(tan(radians)) / M_PI) * 0.5 * (1u << zoom);
}

static void set_status(bool loading, uint8_t loaded, uint8_t required, const char *message)
{
    portENTER_CRITICAL(&s_state_lock);
    s_status.loading = loading;
    s_status.loaded_tiles = loaded;
    s_status.required_tiles = required;
    if (message) strlcpy(s_status.message, message, sizeof(s_status.message));
    portEXIT_CRITICAL(&s_state_lock);
}

static bool make_directory(const char *path)
{
    return mkdir(path, 0775) == 0 || errno == EEXIST;
}

static bool cache_path(char *path, size_t size, uint8_t zoom, int32_t x, int32_t y, bool create)
{
    char zoom_dir[48];
    char x_dir[64];
    snprintf(zoom_dir, sizeof(zoom_dir), MAP_CACHE_ROOT "/%u", zoom);
    snprintf(x_dir, sizeof(x_dir), "%s/%ld", zoom_dir, (long)x);
    if (create && (!make_directory(BSP_SD_MOUNT_POINT "/ORB") ||
                   !make_directory(MAP_CACHE_ROOT) ||
                   !make_directory(zoom_dir) || !make_directory(x_dir))) return false;
    snprintf(path, size, "%s/%ld.png", x_dir, (long)y);
    return true;
}

static uint8_t *read_cached_tile(uint8_t zoom, int32_t x, int32_t y, size_t *size)
{
    char path[96];
    if (!cache_path(path, sizeof(path), zoom, x, y, false)) return NULL;
    struct stat info;
    if (stat(path, &info) != 0 || info.st_size < 24 || info.st_size > MAP_TILE_LIMIT) return NULL;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    uint8_t *data = heap_caps_malloc((size_t)info.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) data = malloc((size_t)info.st_size);
    const size_t read = data ? fread(data, 1, (size_t)info.st_size, file) : 0;
    fclose(file);
    if (!data || read != (size_t)info.st_size || memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0) {
        free(data);
        return NULL;
    }
    *size = read;
    return data;
}

static void write_cached_tile(uint8_t zoom, int32_t x, int32_t y, const uint8_t *data, size_t size)
{
    char path[96];
    char temporary[104];
    if (!cache_path(path, sizeof(path), zoom, x, y, true)) return;
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *file = fopen(temporary, "wb");
    if (!file) return;
    const bool complete = fwrite(data, 1, size, file) == size && fflush(file) == 0;
    fclose(file);
    if (complete) rename(temporary, path);
    else unlink(temporary);
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_buffer_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !response || response->failed) return ESP_OK;
    if (response->size + event->data_len > MAP_TILE_LIMIT) {
        response->failed = true;
        return ESP_ERR_NO_MEM;
    }
    if (response->size + event->data_len > response->capacity) {
        size_t capacity = response->capacity ? response->capacity * 2 : 16384;
        while (capacity < response->size + event->data_len) capacity *= 2;
        if (capacity > MAP_TILE_LIMIT) capacity = MAP_TILE_LIMIT;
        uint8_t *grown = heap_caps_realloc(response->data, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    return ESP_OK;
}

static uint8_t *download_tile(uint8_t zoom, int32_t x, int32_t y, size_t *size)
{
    char url[112];
    snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%u/%ld/%ld.png", zoom, (long)x, (long)y);
    response_buffer_t response = {0};
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .user_agent = "SpacePC-Orb/0.2 (+https://spacepc.dev; contact: hello@spacepc.dev)",
        .keep_alive_enable = true,
    };
    if (!orb_net_take(pdMS_TO_TICKS(30000))) return NULL;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        orb_net_give();
        return NULL;
    }
    esp_http_client_set_header(client, "Accept", "image/png");
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    orb_net_give();
    if (err != ESP_OK || status != 200 || response.failed || response.size < 24 ||
        memcmp(response.data, "\x89PNG\r\n\x1a\n", 8) != 0) {
        ESP_LOGW(TAG, "Tile %u/%ld/%ld failed: %s, HTTP %d", zoom, (long)x, (long)y,
                 esp_err_to_name(err), status);
        free(response.data);
        return NULL;
    }
    *size = response.size;
    return response.data;
}

/* Convert the cached OSM artwork to a native, high-contrast dark RGB565 tile.
 * Keeping this local avoids coupling the product to a second map provider and
 * makes cached tiles work in exactly the same style while offline. */
static uint8_t *decode_dark_tile(const uint8_t *png_data, size_t png_size, size_t *image_size)
{
    png_image image = {0};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, png_data, png_size)) return NULL;
    if (image.width != ORB_MAP_TILE_SIZE || image.height != ORB_MAP_TILE_SIZE) {
        png_image_free(&image);
        return NULL;
    }

    image.format = PNG_FORMAT_RGB;
    const size_t rgb_size = PNG_IMAGE_SIZE(image);
    uint8_t *rgb = heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb) rgb = malloc(rgb_size);
    if (!rgb || !png_image_finish_read(&image, NULL, rgb, 0, NULL)) {
        ESP_LOGW(TAG, "PNG decode failed: %s", image.message);
        free(rgb);
        png_image_free(&image);
        return NULL;
    }

    const size_t pixel_count = ORB_MAP_TILE_SIZE * ORB_MAP_TILE_SIZE;
    uint16_t *dark = heap_caps_malloc(pixel_count * sizeof(*dark), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dark) dark = malloc(pixel_count * sizeof(*dark));
    if (!dark) {
        free(rgb);
        png_image_free(&image);
        return NULL;
    }

    for (size_t i = 0; i < pixel_count; i++) {
        const uint8_t red = rgb[i * 3];
        const uint8_t green = rgb[i * 3 + 1];
        const uint8_t blue = rgb[i * 3 + 2];
        const uint16_t luminance = (54u * red + 183u * green + 19u * blue) >> 8;
        const uint16_t ink = 255u - luminance;
        /* Pale OSM land becomes deep navy; labels, borders and road casings
         * become progressively brighter blue-grey. */
        const uint8_t out_red = (uint8_t)(6u + ink * 32u / 100u);
        const uint8_t out_green = (uint8_t)(12u + ink * 39u / 100u);
        const uint8_t out_blue = (uint8_t)(23u + ink * 48u / 100u);
        dark[i] = (uint16_t)(((out_red & 0xf8u) << 8) |
                             ((out_green & 0xfcu) << 3) |
                             (out_blue >> 3));
    }

    free(rgb);
    png_image_free(&image);
    *image_size = pixel_count * sizeof(*dark);
    return (uint8_t *)dark;
}

static void publish_tile(orb_map_tile_t *tile)
{
    if (xQueueSend(s_result_queue, tile, 0) == pdTRUE) return;
    orb_map_tile_t discarded = {0};
    if (xQueueReceive(s_result_queue, &discarded, 0) == pdTRUE) orb_map_release_tile(&discarded);
    if (xQueueSend(s_result_queue, tile, 0) != pdTRUE) orb_map_release_tile(tile);
}

static bool newer_request(map_request_t *request)
{
    map_request_t next;
    bool found = false;
    while (xQueueReceive(s_request_queue, &next, 0) == pdTRUE) {
        *request = next;
        found = true;
    }
    return found;
}

static void map_task(void *argument)
{
    (void)argument;
    static const int8_t offsets[ORB_MAP_VIEW_TILE_COUNT][2] = {
        {0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1},
        {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
        {0, -2}, {0, 2}, {-1, -2}, {1, -2}, {-1, 2}, {1, 2},
    };
    map_request_t request;
    while (xQueueReceive(s_request_queue, &request, portMAX_DELAY) == pdTRUE) {
        bool retry = true;
        while (retry) {
            retry = false;
            const int32_t dimension = 1 << request.zoom;
            const int32_t center_x = (int32_t)floor(longitude_to_tile_x(request.longitude, request.zoom));
            const int32_t center_y = (int32_t)floor(latitude_to_tile_y(request.latitude, request.zoom));
            uint8_t loaded = 0;
            bool missing = false;
            set_status(true, 0, ORB_MAP_VIEW_TILE_COUNT, "Loading map tiles...");
            for (size_t i = 0; i < ORB_MAP_VIEW_TILE_COUNT; i++) {
                if (newer_request(&request)) {
                    retry = true;
                    break;
                }
                int32_t x = center_x + offsets[i][0];
                const int32_t y = center_y + offsets[i][1];
                x = (x % dimension + dimension) % dimension;
                if (y < 0 || y >= dimension) continue;
                size_t size = 0;
                bool from_cache = true;
                uint8_t *data = read_cached_tile(request.zoom, x, y, &size);
                if (!data) {
                    orb_wifi_status_t wifi;
                    orb_wifi_get_status(&wifi);
                    orb_map_status_t status;
                    orb_map_get_status(&status);
                    if (wifi.connected && status.sd_mounted) {
                        from_cache = false;
                        data = download_tile(request.zoom, x, y, &size);
                        if (data) write_cached_tile(request.zoom, x, y, data, size);
                        vTaskDelay(pdMS_TO_TICKS(120));
                    }
                }
                if (!data) {
                    missing = true;
                    continue;
                }
                size_t image_size = 0;
                uint8_t *image_data = decode_dark_tile(data, size, &image_size);
                free(data);
                if (!image_data) {
                    missing = true;
                    continue;
                }
                orb_map_tile_t tile = {
                    .generation = request.generation,
                    .zoom = request.zoom,
                    .x = x,
                    .y = y,
                    .image_data = image_data,
                    .image_size = image_size,
                    .from_cache = from_cache,
                };
                publish_tile(&tile);
                loaded++;
                char message[48];
                snprintf(message, sizeof(message), "Map %u/%u%s", loaded, ORB_MAP_VIEW_TILE_COUNT,
                         from_cache ? " cached" : " live");
                set_status(true, loaded, ORB_MAP_VIEW_TILE_COUNT, message);
            }
            if (retry) continue;
            if (!missing) {
                set_status(false, loaded, ORB_MAP_VIEW_TILE_COUNT, "Map ready");
                break;
            }
            orb_wifi_status_t wifi;
            orb_wifi_get_status(&wifi);
            orb_map_status_t status;
            orb_map_get_status(&status);
            set_status(false, loaded, ORB_MAP_VIEW_TILE_COUNT, !status.sd_mounted ? "Insert microSD for maps" :
                       (wifi.connected ? "Map service unavailable" : "Waiting for WiFi"));
            const TickType_t retry_delay = pdMS_TO_TICKS(wifi.connected ? 30000 : 1500);
            if (xQueueReceive(s_request_queue, &request, retry_delay) == pdTRUE) retry = true;
            else retry = true;
        }
    }
}

esp_err_t orb_map_init(void)
{
    if (s_status.initialized) return ESP_OK;
    ESP_RETURN_ON_ERROR(orb_net_init(), TAG, "network lock");
    s_request_queue = xQueueCreate(1, sizeof(map_request_t));
    s_result_queue = xQueueCreate(20, sizeof(orb_map_tile_t));
    if (!s_request_queue || !s_result_queue) return ESP_ERR_NO_MEM;
    const esp_err_t mount = bsp_sdcard_mount();
    const bool mounted = mount == ESP_OK || mount == ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_state_lock);
    s_status.initialized = true;
    s_status.sd_mounted = mounted;
    strlcpy(s_status.message, mounted ? "Map cache ready" : "Insert microSD for maps", sizeof(s_status.message));
    portEXIT_CRITICAL(&s_state_lock);
    if (!mounted) ESP_LOGW(TAG, "microSD map cache unavailable: %s", esp_err_to_name(mount));
    else {
        make_directory(BSP_SD_MOUNT_POINT "/ORB");
        make_directory(MAP_CACHE_ROOT);
    }
    if (xTaskCreate(map_task, "orb_map_tiles", 14336, NULL, 3, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

uint32_t orb_map_request(double latitude, double longitude, uint8_t zoom)
{
    if (!s_request_queue) return 0;
    if (zoom < ORB_MAP_MIN_ZOOM) zoom = ORB_MAP_MIN_ZOOM;
    if (zoom > ORB_MAP_MAX_ZOOM) zoom = ORB_MAP_MAX_ZOOM;
    portENTER_CRITICAL(&s_state_lock);
    const uint32_t generation = ++s_generation;
    portEXIT_CRITICAL(&s_state_lock);
    const map_request_t request = {
        .generation = generation,
        .latitude = clamp_latitude(latitude),
        .longitude = longitude,
        .zoom = zoom,
    };
    xQueueOverwrite(s_request_queue, &request);
    return generation;
}

bool orb_map_take_tile(orb_map_tile_t *tile)
{
    return tile && s_result_queue && xQueueReceive(s_result_queue, tile, 0) == pdTRUE;
}

void orb_map_release_tile(orb_map_tile_t *tile)
{
    if (!tile) return;
    free(tile->image_data);
    memset(tile, 0, sizeof(*tile));
}

void orb_map_get_status(orb_map_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_state_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_state_lock);
}
