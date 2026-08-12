#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ORB_MAP_TILE_SIZE 256
#define ORB_MAP_VIEW_TILE_COUNT 15
#define ORB_MAP_MIN_ZOOM 4
#define ORB_MAP_MAX_ZOOM 14

typedef struct {
    uint32_t generation;
    uint8_t zoom;
    int32_t x;
    int32_t y;
    uint8_t *image_data;
    size_t image_size;
    bool from_cache;
} orb_map_tile_t;

typedef struct {
    bool initialized;
    bool sd_mounted;
    bool loading;
    uint8_t loaded_tiles;
    uint8_t required_tiles;
    char message[48];
} orb_map_status_t;

esp_err_t orb_map_init(void);
uint32_t orb_map_request(double latitude, double longitude, uint8_t zoom);
bool orb_map_take_tile(orb_map_tile_t *tile);
void orb_map_release_tile(orb_map_tile_t *tile);
void orb_map_get_status(orb_map_status_t *status);
