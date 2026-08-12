#include "orb_ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"
#include "lvgl.h"
#include "orb_data.h"
#include "orb_geocode.h"
#include "orb_map.h"
#include "orb_route.h"
#include "orb_settings.h"
#include "orb_timezone.h"
#include "orb_wifi.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { PAGE_SKY, PAGE_ORBIT, PAGE_ISS, PAGE_COUNT };
enum { COLOR_BG = 0x050812, COLOR_PANEL = 0x0d1422, COLOR_LINE = 0x26354c,
       COLOR_TEXT = 0xf2f6ff, COLOR_MUTED = 0x7f8ca5, COLOR_CYAN = 0x57e4ff,
       COLOR_PURPLE = 0x9b7cff, COLOR_GREEN = 0x55e6ad, COLOR_ORANGE = 0xffb25d,
       COLOR_YELLOW = 0xffd84d, COLOR_YELLOW_SELECTED = 0xffff8a };

static orb_settings_t *s_settings;
static lv_obj_t *s_root;
static lv_obj_t *s_track;
static lv_obj_t *s_pages[PAGE_COUNT];
static lv_obj_t *s_page_dots[PAGE_COUNT];
static lv_obj_t *s_live_dot[PAGE_COUNT];
static lv_obj_t *s_live_text[PAGE_COUNT];
static lv_obj_t *s_clock[PAGE_COUNT];
static int s_header_index;
static int s_page;

static lv_obj_t *s_sky_plot;
static lv_obj_t *s_map_status_pill;
static lv_obj_t *s_map_status;
static lv_obj_t *s_map_zoom_label;
static lv_obj_t *s_observer_marker;
static lv_obj_t *s_aircraft_markers[ORB_MAX_AIRCRAFT];
static lv_obj_t *s_aircraft_outlines[ORB_MAX_AIRCRAFT];
static lv_obj_t *s_aircraft_spines[ORB_MAX_AIRCRAFT];
static lv_obj_t *s_aircraft_names[ORB_MAX_AIRCRAFT];
static lv_obj_t *s_aircraft_count;
static lv_obj_t *s_plane_call;
static lv_obj_t *s_plane_type;
static lv_obj_t *s_plane_altitude;
static lv_obj_t *s_plane_speed;
static lv_obj_t *s_plane_distance;
static lv_obj_t *s_plane_route;
static lv_obj_t *s_plane_route_names;
static lv_obj_t *s_plane_duration;
static lv_obj_t *s_flight_detail;
static int s_selected_aircraft = -1;
static char s_route_callsign[12];

#define MAP_WIDTH 480
#define MAP_HEIGHT 800
#define MAP_TILE_SLOTS ORB_MAP_VIEW_TILE_COUNT
#define ISS_MAP_WIDTH 448
#define ISS_MAP_HEIGHT 438
#define ISS_MAP_ZOOM ORB_MAP_MIN_ZOOM

typedef struct {
    lv_obj_t *image;
    lv_image_dsc_t descriptor;
    uint8_t *image_data;
    size_t image_size;
    int32_t tile_x;
    int32_t tile_y;
    uint8_t zoom;
    bool loaded;
} map_tile_slot_t;

static map_tile_slot_t s_map_tiles[MAP_TILE_SLOTS];
static map_tile_slot_t s_iss_map_tiles[MAP_TILE_SLOTS];
static double s_map_center_lat;
static double s_map_center_lon;
static uint8_t s_map_zoom;
static uint32_t s_map_generation;
static bool s_map_dragging;
static int s_map_drag_distance;
static int s_map_press_y;
static uint16_t s_map_radius_nm;

static lv_obj_t *s_orbit_markers[ORB_MAX_SATELLITES];
static lv_obj_t *s_orbit_names[4];
static lv_obj_t *s_orbit_positions[4];
static lv_obj_t *s_catalog_count;

static lv_obj_t *s_iss_marker;
static lv_obj_t *s_iss_halo;
static lv_obj_t *s_iss_map_plot;
static lv_obj_t *s_iss_trail[14];
static double s_iss_trail_lat[14];
static double s_iss_trail_lon[14];
static bool s_iss_trail_valid[14];
static int s_iss_trail_head;
static int64_t s_last_trail_ms;
static int64_t s_last_iss_map_request_ms;
static double s_display_iss_lat;
static double s_display_iss_lon;
static double s_iss_map_center_lat;
static double s_iss_map_center_lon;
static uint32_t s_iss_map_generation;
static lv_obj_t *s_iss_latlon;
static lv_obj_t *s_iss_altitude;
static lv_obj_t *s_iss_velocity;
static lv_obj_t *s_iss_visibility;
static lv_obj_t *s_iss_range;

static lv_obj_t *s_settings_overlay;
static lv_obj_t *s_settings_wifi;
static lv_obj_t *s_settings_location;
static lv_obj_t *s_unit_buttons[3][3];
static lv_obj_t *s_config_dialog;
static lv_obj_t *s_config_keyboard;
static lv_obj_t *s_field_ssid;
static lv_obj_t *s_field_password;
static lv_obj_t *s_field_city;
static lv_obj_t *s_field_lat;
static lv_obj_t *s_field_lon;
static bool s_geocode_pending;
static char s_geocode_message[80];
static int64_t s_geocode_message_until;
static int64_t s_timezone_request_ms;

#define TIMEZONE_REFRESH_MS (6LL * 60 * 60 * 1000)

static void request_iss_map_tiles(void);

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    return object;
}

static void format_distance(char *text, size_t size, double nautical_miles, int decimals)
{
    if (s_settings->distance_unit == ORB_DISTANCE_KILOMETRES) {
        snprintf(text, size, decimals ? "%.1f km" : "%.0f km", nautical_miles * 1.852);
    } else if (s_settings->distance_unit == ORB_DISTANCE_MILES) {
        snprintf(text, size, decimals ? "%.1f mi" : "%.0f mi", nautical_miles * 1.15077945);
    } else {
        snprintf(text, size, decimals ? "%.1f nm" : "%.0f nm", nautical_miles);
    }
}

static void format_altitude(char *text, size_t size, double feet)
{
    if (s_settings->altitude_unit == ORB_ALTITUDE_METRES) snprintf(text, size, "%.0f m", feet * 0.3048);
    else snprintf(text, size, "%.0f ft", feet);
}

static void format_speed(char *text, size_t size, double knots)
{
    if (s_settings->speed_unit == ORB_SPEED_KMH) snprintf(text, size, "%.0f km/h", knots * 1.852);
    else if (s_settings->speed_unit == ORB_SPEED_MPH) snprintf(text, size, "%.0f mph", knots * 1.15077945);
    else snprintf(text, size, "%.0f kt", knots);
}

static void format_duration(char *text, size_t size, uint16_t minutes)
{
    if (!minutes) strlcpy(text, "--", size);
    else if (minutes < 60) snprintf(text, size, "~%u min", minutes);
    else snprintf(text, size, "~%uh %02um", minutes / 60, minutes % 60);
}

static void plain(lv_obj_t *object)
{
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(COLOR_LINE), 0);
    lv_obj_set_style_radius(object, 22, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *text, int x, int y, int width, uint32_t color)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 34);
    lv_obj_set_style_radius(button, 17, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *caption = label(button, text, &lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_center(caption);
    return button;
}

static lv_obj_t *line(lv_obj_t *parent, lv_point_precise_t *points, uint32_t count, uint32_t color, int width, lv_opa_t opacity)
{
    lv_obj_t *object = lv_line_create(parent);
    lv_line_set_points(object, points, count);
    lv_obj_set_style_line_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(object, width, 0);
    lv_obj_set_style_line_opa(object, opacity, 0);
    return object;
}

static void animate_x(void *object, int32_t value) { lv_obj_set_x(object, value); }
static void animate_y(void *object, int32_t value) { lv_obj_set_y(object, value); }

static void delete_after_animation(lv_anim_t *animation)
{
    lv_obj_delete((lv_obj_t *)animation->var);
}

static void switch_page(int page)
{
    if (page < 0) page = 0;
    if (page >= PAGE_COUNT) page = PAGE_COUNT - 1;
    if (page == s_page && lv_obj_get_x(s_track) == -page * 480) return;
    s_page = page;
    if (page == PAGE_ISS) request_iss_map_tiles();
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_track);
    lv_anim_set_exec_cb(&animation, animate_x);
    lv_anim_set_values(&animation, lv_obj_get_x(s_track), -page * 480);
    lv_anim_set_duration(&animation, 360);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_set_style_bg_color(s_page_dots[i], lv_color_hex(i == page ? COLOR_CYAN : 0x344158), 0);
        lv_obj_set_size(s_page_dots[i], i == page ? 20 : 7, 7);
    }
}

static void nav_click(lv_event_t *event)
{
    switch_page((int)(intptr_t)lv_event_get_user_data(event));
}

static void settings_close(void)
{
    if (!s_settings_overlay) return;
    lv_obj_t *overlay = s_settings_overlay;
    s_settings_overlay = NULL;
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, overlay);
    lv_anim_set_exec_cb(&animation, animate_y);
    lv_anim_set_values(&animation, lv_obj_get_y(overlay), -800);
    lv_anim_set_duration(&animation, 280);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&animation, delete_after_animation);
    lv_anim_start(&animation);
}

static void gesture_event(lv_event_t *event)
{
    const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
    if (s_settings_overlay) {
        if (direction == LV_DIR_TOP) settings_close();
        return;
    }
    if (direction == LV_DIR_LEFT) switch_page(s_page + 1);
    else if (direction == LV_DIR_RIGHT) switch_page(s_page - 1);
    else if (direction == LV_DIR_BOTTOM) {
        extern void orb_ui_open_settings(void);
        orb_ui_open_settings();
    }
    (void)event;
}

static void header(lv_obj_t *screen)
{
    const int index = s_header_index++;
    lv_obj_t *brand = label(screen, "ORB", &lv_font_montserrat_24, COLOR_TEXT);
    lv_obj_set_pos(brand, 22, 17);
    lv_obj_t *tag = label(screen, "LIVE SKY", &lv_font_montserrat_14, COLOR_CYAN);
    lv_obj_set_pos(tag, 84, 24);

    s_live_dot[index] = lv_obj_create(screen);
    lv_obj_set_pos(s_live_dot[index], 330, 24);
    lv_obj_set_size(s_live_dot[index], 9, 9);
    lv_obj_set_style_radius(s_live_dot[index], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_live_dot[index], lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_set_style_border_width(s_live_dot[index], 0, 0);
    s_live_text[index] = label(screen, "DEMO", &lv_font_montserrat_14, COLOR_ORANGE);
    lv_obj_set_pos(s_live_text[index], 346, 19);
    s_clock[index] = label(screen, "--:--", &lv_font_montserrat_18, COLOR_MUTED);
    lv_obj_set_pos(s_clock[index], 414, 18);
}

static void sky_status_overlay(lv_obj_t *screen)
{
    const int index = s_header_index++;
    s_live_dot[index] = lv_obj_create(screen);
    lv_obj_set_pos(s_live_dot[index], 18, 22);
    lv_obj_set_size(s_live_dot[index], 9, 9);
    lv_obj_set_style_radius(s_live_dot[index], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_live_dot[index], lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_set_style_border_width(s_live_dot[index], 0, 0);
    s_live_text[index] = label(screen, "DEMO", &lv_font_montserrat_14, COLOR_ORANGE);
    lv_obj_set_pos(s_live_text[index], 34, 17);
    s_clock[index] = label(screen, "--:--", &lv_font_montserrat_18, COLOR_TEXT);
    lv_obj_set_pos(s_clock[index], 414, 15);
}

static void create_page_dots(void)
{
    static const char *names[] = {"SKY", "ORBIT", "ISS"};
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *hit = lv_obj_create(s_root);
        lv_obj_set_pos(hit, 145 + i * 64, 756);
        lv_obj_set_size(hit, 60, 30);
        plain(hit);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hit, nav_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *caption = label(hit, names[i], &lv_font_montserrat_14, i == 0 ? COLOR_TEXT : COLOR_MUTED);
        lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, 0);
        s_page_dots[i] = lv_obj_create(hit);
        lv_obj_align(s_page_dots[i], LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_size(s_page_dots[i], i == 0 ? 20 : 7, 7);
        lv_obj_set_style_radius(s_page_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_page_dots[i], lv_color_hex(i == 0 ? COLOR_CYAN : 0x344158), 0);
        lv_obj_set_style_border_width(s_page_dots[i], 0, 0);
    }
}

static double map_world_size(uint8_t zoom)
{
    return (double)ORB_MAP_TILE_SIZE * (1u << zoom);
}

static double map_lon_to_world_x(double longitude, uint8_t zoom)
{
    return (longitude + 180.0) / 360.0 * map_world_size(zoom);
}

static double map_lat_to_world_y(double latitude, uint8_t zoom)
{
    if (latitude > 85.05112878) latitude = 85.05112878;
    if (latitude < -85.05112878) latitude = -85.05112878;
    const double radians = latitude * M_PI / 180.0;
    return (1.0 - asinh(tan(radians)) / M_PI) * 0.5 * map_world_size(zoom);
}

static double map_world_x_to_lon(double x, uint8_t zoom)
{
    const double world = map_world_size(zoom);
    while (x < 0) x += world;
    while (x >= world) x -= world;
    return x / world * 360.0 - 180.0;
}

static double map_world_y_to_lat(double y, uint8_t zoom)
{
    const double world = map_world_size(zoom);
    if (y < 0) y = 0;
    if (y > world) y = world;
    return atan(sinh(M_PI * (1.0 - 2.0 * y / world))) * 180.0 / M_PI;
}

static double map_wrapped_delta(double target, double center, double world)
{
    double delta = target - center;
    while (delta > world * 0.5) delta -= world;
    while (delta < -world * 0.5) delta += world;
    return delta;
}

static void position_map_tiles(void)
{
    const double world = map_world_size(s_map_zoom);
    const double center_x = map_lon_to_world_x(s_map_center_lon, s_map_zoom);
    const double center_y = map_lat_to_world_y(s_map_center_lat, s_map_zoom);
    for (int i = 0; i < MAP_TILE_SLOTS; i++) {
        map_tile_slot_t *slot = &s_map_tiles[i];
        if (!slot->loaded || slot->zoom != s_map_zoom) {
            lv_obj_add_flag(slot->image, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const double tile_x = slot->tile_x * ORB_MAP_TILE_SIZE;
        const double tile_y = slot->tile_y * ORB_MAP_TILE_SIZE;
        const int x = (int)lround(MAP_WIDTH * 0.5 + map_wrapped_delta(tile_x, center_x, world));
        const int y = (int)lround(MAP_HEIGHT * 0.5 + tile_y - center_y);
        lv_obj_set_pos(slot->image, x, y);
        lv_obj_remove_flag(slot->image, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool map_tile_needed(const map_tile_slot_t *slot)
{
    if (!slot->loaded || slot->zoom != s_map_zoom) return false;
    const int dimension = 1 << s_map_zoom;
    const int center_x = (int)floor(map_lon_to_world_x(s_map_center_lon, s_map_zoom) / ORB_MAP_TILE_SIZE);
    const int center_y = (int)floor(map_lat_to_world_y(s_map_center_lat, s_map_zoom) / ORB_MAP_TILE_SIZE);
    int dx = slot->tile_x - center_x;
    if (dx > dimension / 2) dx -= dimension;
    if (dx < -dimension / 2) dx += dimension;
    return abs(dx) <= 1 && abs(slot->tile_y - center_y) <= 2;
}

static void release_map_slot(map_tile_slot_t *slot)
{
    if (!slot->loaded) return;
    lv_obj_add_flag(slot->image, LV_OBJ_FLAG_HIDDEN);
    free(slot->image_data);
    memset(&slot->descriptor, 0, sizeof(slot->descriptor));
    slot->image_data = NULL;
    slot->image_size = 0;
    slot->loaded = false;
}

static void accept_map_tile(orb_map_tile_t *tile)
{
    if (!tile->image_data || tile->generation != s_map_generation || tile->zoom != s_map_zoom) {
        orb_map_release_tile(tile);
        return;
    }
    for (int i = 0; i < MAP_TILE_SLOTS; i++) {
        map_tile_slot_t *slot = &s_map_tiles[i];
        if (slot->loaded && slot->zoom == tile->zoom && slot->tile_x == tile->x && slot->tile_y == tile->y) {
            orb_map_release_tile(tile);
            return;
        }
    }
    map_tile_slot_t *slot = NULL;
    for (int i = 0; i < MAP_TILE_SLOTS; i++) {
        if (!s_map_tiles[i].loaded) {
            slot = &s_map_tiles[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < MAP_TILE_SLOTS; i++) {
            if (!map_tile_needed(&s_map_tiles[i])) {
                slot = &s_map_tiles[i];
                break;
            }
        }
    }
    if (!slot) {
        orb_map_release_tile(tile);
        return;
    }
    release_map_slot(slot);
    slot->image_data = tile->image_data;
    slot->image_size = tile->image_size;
    slot->tile_x = tile->x;
    slot->tile_y = tile->y;
    slot->zoom = tile->zoom;
    slot->loaded = true;
    slot->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot->descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    slot->descriptor.header.w = ORB_MAP_TILE_SIZE;
    slot->descriptor.header.h = ORB_MAP_TILE_SIZE;
    slot->descriptor.header.stride = ORB_MAP_TILE_SIZE * sizeof(uint16_t);
    slot->descriptor.data_size = tile->image_size;
    slot->descriptor.data = tile->image_data;
    lv_image_set_src(slot->image, &slot->descriptor);
    tile->image_data = NULL;
    position_map_tiles();
}

static bool iss_map_tile_needed(const map_tile_slot_t *slot)
{
    if (!slot->loaded || slot->zoom != ISS_MAP_ZOOM) return false;
    const int dimension = 1 << ISS_MAP_ZOOM;
    const int center_x = (int)floor(map_lon_to_world_x(s_iss_map_center_lon, ISS_MAP_ZOOM) /
                                    ORB_MAP_TILE_SIZE);
    const int center_y = (int)floor(map_lat_to_world_y(s_iss_map_center_lat, ISS_MAP_ZOOM) /
                                    ORB_MAP_TILE_SIZE);
    int dx = slot->tile_x - center_x;
    if (dx > dimension / 2) dx -= dimension;
    if (dx < -dimension / 2) dx += dimension;
    return abs(dx) <= 1 && abs(slot->tile_y - center_y) <= 2;
}

static void position_iss_map_tiles(void)
{
    const double world = map_world_size(ISS_MAP_ZOOM);
    const double center_x = map_lon_to_world_x(s_iss_map_center_lon, ISS_MAP_ZOOM);
    const double center_y = map_lat_to_world_y(s_iss_map_center_lat, ISS_MAP_ZOOM);
    for (int i = 0; i < MAP_TILE_SLOTS; i++) {
        map_tile_slot_t *slot = &s_iss_map_tiles[i];
        if (!slot->loaded || slot->zoom != ISS_MAP_ZOOM) {
            lv_obj_add_flag(slot->image, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const double tile_x = slot->tile_x * ORB_MAP_TILE_SIZE;
        const double tile_y = slot->tile_y * ORB_MAP_TILE_SIZE;
        const int x = (int)lround(ISS_MAP_WIDTH * 0.5 +
                                  map_wrapped_delta(tile_x, center_x, world));
        const int y = (int)lround(ISS_MAP_HEIGHT * 0.5 + tile_y - center_y);
        lv_obj_set_pos(slot->image, x, y);
        lv_obj_remove_flag(slot->image, LV_OBJ_FLAG_HIDDEN);
    }
}

static void accept_iss_map_tile(orb_map_tile_t *tile)
{
    if (!tile->image_data || tile->generation != s_iss_map_generation ||
        tile->zoom != ISS_MAP_ZOOM) {
        orb_map_release_tile(tile);
        return;
    }
    for (int i = 0; i < MAP_TILE_SLOTS; i++) {
        map_tile_slot_t *slot = &s_iss_map_tiles[i];
        if (slot->loaded && slot->zoom == tile->zoom &&
            slot->tile_x == tile->x && slot->tile_y == tile->y) {
            orb_map_release_tile(tile);
            return;
        }
    }
    map_tile_slot_t *slot = NULL;
    for (int i = 0; i < MAP_TILE_SLOTS; i++) {
        if (!s_iss_map_tiles[i].loaded) {
            slot = &s_iss_map_tiles[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < MAP_TILE_SLOTS; i++) {
            if (!iss_map_tile_needed(&s_iss_map_tiles[i])) {
                slot = &s_iss_map_tiles[i];
                break;
            }
        }
    }
    if (!slot) {
        orb_map_release_tile(tile);
        return;
    }
    release_map_slot(slot);
    slot->image_data = tile->image_data;
    slot->image_size = tile->image_size;
    slot->tile_x = tile->x;
    slot->tile_y = tile->y;
    slot->zoom = tile->zoom;
    slot->loaded = true;
    slot->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot->descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    slot->descriptor.header.w = ORB_MAP_TILE_SIZE;
    slot->descriptor.header.h = ORB_MAP_TILE_SIZE;
    slot->descriptor.header.stride = ORB_MAP_TILE_SIZE * sizeof(uint16_t);
    slot->descriptor.data_size = tile->image_size;
    slot->descriptor.data = tile->image_data;
    lv_image_set_src(slot->image, &slot->descriptor);
    tile->image_data = NULL;
    position_iss_map_tiles();
}

static void request_iss_map_tiles(void)
{
    if (!s_iss_map_plot) return;
    orb_data_snapshot_t data;
    orb_data_get_snapshot(&data);
    s_iss_map_center_lat = data.iss_latitude;
    s_iss_map_center_lon = data.iss_longitude;
    position_iss_map_tiles();
    s_iss_map_generation = orb_map_request(s_iss_map_center_lat, s_iss_map_center_lon,
                                           ISS_MAP_ZOOM);
    s_last_iss_map_request_ms = esp_timer_get_time() / 1000;
}

static uint16_t map_visible_radius_nm(void)
{
    const double latitude = fmin(85.0, fmax(-85.0, s_map_center_lat));
    const double metres_per_pixel = 40075016.686 * cos(latitude * M_PI / 180.0) /
                                    ((double)ORB_MAP_TILE_SIZE * (1u << s_map_zoom));
    const double half_diagonal = hypot(MAP_WIDTH * 0.5, MAP_HEIGHT * 0.5);
    int radius = (int)ceil(half_diagonal * metres_per_pixel * 1.12 / 1852.0);
    if (radius < 10) radius = 10;
    if (radius > 250) radius = 250;
    return (uint16_t)radius;
}

static void request_map_tiles(void)
{
    s_map_generation = orb_map_request(s_map_center_lat, s_map_center_lon, s_map_zoom);
    s_map_radius_nm = map_visible_radius_nm();
    s_settings->aircraft_radius_nm = s_map_radius_nm;
    orb_data_set_observer(s_map_center_lat, s_map_center_lon, s_map_radius_nm);
    char text[16];
    snprintf(text, sizeof(text), "Z%u", s_map_zoom);
    lv_label_set_text(s_map_zoom_label, text);
}

static void aircraft_select(lv_event_t *event)
{
    s_selected_aircraft = (int)(intptr_t)lv_event_get_user_data(event);
    lv_obj_remove_flag(s_flight_detail, LV_OBJ_FLAG_HIDDEN);
    orb_data_snapshot_t data;
    orb_data_get_snapshot(&data);
    if (s_selected_aircraft < (int)data.aircraft_count) {
        const char *callsign = data.aircraft[s_selected_aircraft].callsign;
        strlcpy(s_route_callsign, callsign, sizeof(s_route_callsign));
        lv_label_set_text(s_plane_route, "LOOKING UP ROUTE...");
        lv_label_set_text(s_plane_route_names, "");
        lv_label_set_text(s_plane_duration, "--");
        if (!orb_route_request(callsign, data.aircraft[s_selected_aircraft].latitude,
                               data.aircraft[s_selected_aircraft].longitude)) {
            lv_label_set_text(s_plane_route, "ROUTE LOOKUP OFFLINE");
        }
    }
    lv_event_stop_bubbling(event);
}

static void map_touch_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        s_map_dragging = true;
        s_map_drag_distance = 0;
        lv_point_t point = {0};
        lv_indev_get_point(lv_indev_active(), &point);
        s_map_press_y = point.y;
    } else if (code == LV_EVENT_PRESSING && s_map_dragging) {
        lv_point_t vector = {0};
        lv_indev_get_vect(lv_indev_active(), &vector);
        if (vector.x || vector.y) {
            const double center_x = map_lon_to_world_x(s_map_center_lon, s_map_zoom) - vector.x;
            const double center_y = map_lat_to_world_y(s_map_center_lat, s_map_zoom) - vector.y;
            s_map_center_lon = map_world_x_to_lon(center_x, s_map_zoom);
            s_map_center_lat = map_world_y_to_lat(center_y, s_map_zoom);
            s_map_drag_distance += abs(vector.x) + abs(vector.y);
            position_map_tiles();
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_map_dragging && s_map_drag_distance > 4) {
            request_map_tiles();
        } else if (s_map_dragging && code == LV_EVENT_RELEASED) {
            s_selected_aircraft = -1;
            s_route_callsign[0] = '\0';
            lv_obj_add_flag(s_flight_detail, LV_OBJ_FLAG_HIDDEN);
        }
        s_map_dragging = false;
    } else if (code == LV_EVENT_GESTURE && s_map_press_y <= 108) {
        const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
        s_map_dragging = false;
        if (direction == LV_DIR_BOTTOM) {
            extern void orb_ui_open_settings(void);
            orb_ui_open_settings();
        } else if (direction == LV_DIR_LEFT) {
            switch_page(s_page + 1);
        } else if (direction == LV_DIR_RIGHT) {
            switch_page(s_page - 1);
        }
    }
    if (code == LV_EVENT_GESTURE || code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING ||
        code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) lv_event_stop_bubbling(event);
}

static void map_zoom_event(lv_event_t *event)
{
    const int delta = (int)(intptr_t)lv_event_get_user_data(event);
    int zoom = (int)s_map_zoom + delta;
    if (zoom < ORB_MAP_MIN_ZOOM) zoom = ORB_MAP_MIN_ZOOM;
    if (zoom > ORB_MAP_MAX_ZOOM) zoom = ORB_MAP_MAX_ZOOM;
    if (zoom == s_map_zoom) return;
    s_map_zoom = (uint8_t)zoom;
    s_settings->map_zoom = s_map_zoom;
    s_settings->aircraft_radius_nm = map_visible_radius_nm();
    orb_settings_save(s_settings);
    for (int i = 0; i < MAP_TILE_SLOTS; i++) release_map_slot(&s_map_tiles[i]);
    request_map_tiles();
    lv_event_stop_bubbling(event);
}

static void map_home_event(lv_event_t *event)
{
    s_map_center_lat = s_settings->latitude;
    s_map_center_lon = s_settings->longitude;
    position_map_tiles();
    request_map_tiles();
    lv_event_stop_bubbling(event);
}

static lv_obj_t *map_control(lv_obj_t *parent, const char *text, int y, lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, 424, y);
    lv_obj_set_size(button, 42, 42);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x0b1422), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_90, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x536883), 0);
    lv_obj_set_style_shadow_width(button, 10, 0);
    lv_obj_set_style_shadow_opa(button, LV_OPA_30, 0);
    lv_obj_t *caption = label(button, text, &lv_font_montserrat_20, COLOR_TEXT);
    lv_obj_center(caption);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    return button;
}

static const lv_point_precise_t s_aircraft_outline[] = {
    {18, 1}, {22, 14}, {34, 19}, {34, 23}, {22, 20}, {22, 29},
    {28, 34}, {28, 36}, {18, 33}, {8, 36}, {8, 34}, {14, 29},
    {14, 20}, {2, 23}, {2, 19}, {14, 14}, {18, 1},
};

static const lv_point_precise_t s_aircraft_spine[] = {{18, 4}, {18, 32}};

static void create_sky_page(void)
{
    lv_obj_t *page = lv_obj_create(s_track);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, 480, 800);
    plain(page);
    lv_obj_add_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_sky_plot = lv_obj_create(page);
    lv_obj_set_pos(s_sky_plot, 0, 0);
    lv_obj_set_size(s_sky_plot, MAP_WIDTH, MAP_HEIGHT);
    lv_obj_set_style_bg_color(s_sky_plot, lv_color_hex(0x050a12), 0);
    lv_obj_set_style_bg_opa(s_sky_plot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sky_plot, 0, 0);
    lv_obj_set_style_radius(s_sky_plot, 0, 0);
    lv_obj_set_style_pad_all(s_sky_plot, 0, 0);
    lv_obj_remove_flag(s_sky_plot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sky_plot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_sky_plot, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_sky_plot, map_touch_event, LV_EVENT_ALL, NULL);

    for (int i = 0; i < MAP_TILE_SLOTS; i++) {
        s_map_tiles[i].image = lv_image_create(s_sky_plot);
        lv_obj_add_flag(s_map_tiles[i].image, LV_OBJ_FLAG_HIDDEN);
    }

    s_observer_marker = lv_obj_create(s_sky_plot);
    lv_obj_set_size(s_observer_marker, 18, 18);
    lv_obj_set_style_radius(s_observer_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_observer_marker, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_bg_opa(s_observer_marker, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_observer_marker, 3, 0);
    lv_obj_set_style_border_color(s_observer_marker, lv_color_hex(COLOR_TEXT), 0);

    for (int i = 0; i < ORB_MAX_AIRCRAFT; i++) {
        s_aircraft_markers[i] = lv_obj_create(s_sky_plot);
        lv_obj_set_size(s_aircraft_markers[i], 38, 38);
        plain(s_aircraft_markers[i]);
        lv_obj_add_flag(s_aircraft_markers[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_aircraft_markers[i], aircraft_select, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_aircraft_outlines[i] = line(s_aircraft_markers[i], (lv_point_precise_t *)s_aircraft_outline,
                                      sizeof(s_aircraft_outline) / sizeof(s_aircraft_outline[0]),
                                      COLOR_YELLOW, 3, LV_OPA_COVER);
        lv_obj_set_style_line_rounded(s_aircraft_outlines[i], true, 0);
        s_aircraft_spines[i] = line(s_aircraft_markers[i], (lv_point_precise_t *)s_aircraft_spine,
                                    sizeof(s_aircraft_spine) / sizeof(s_aircraft_spine[0]),
                                    COLOR_YELLOW, 2, LV_OPA_COVER);
        lv_obj_set_style_line_rounded(s_aircraft_spines[i], true, 0);
        lv_obj_set_style_transform_pivot_x(s_aircraft_markers[i], 19, 0);
        lv_obj_set_style_transform_pivot_y(s_aircraft_markers[i], 19, 0);
        s_aircraft_names[i] = label(s_sky_plot, "", &lv_font_montserrat_14, COLOR_YELLOW);
        lv_obj_set_style_bg_color(s_aircraft_names[i], lv_color_hex(0x050a12), 0);
        lv_obj_set_style_bg_opa(s_aircraft_names[i], LV_OPA_80, 0);
        lv_obj_add_flag(s_aircraft_markers[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_aircraft_names[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_map_status_pill = lv_obj_create(s_sky_plot);
    lv_obj_set_pos(s_map_status_pill, 10, 58);
    lv_obj_set_size(s_map_status_pill, 184, 34);
    lv_obj_set_style_radius(s_map_status_pill, 17, 0);
    lv_obj_set_style_bg_color(s_map_status_pill, lv_color_hex(0x07101b), 0);
    lv_obj_set_style_bg_opa(s_map_status_pill, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_map_status_pill, 1, 0);
    lv_obj_set_style_border_color(s_map_status_pill, lv_color_hex(0x30445c), 0);
    lv_obj_set_style_pad_all(s_map_status_pill, 0, 0);
    lv_obj_remove_flag(s_map_status_pill, LV_OBJ_FLAG_SCROLLABLE);
    s_map_status = label(s_map_status_pill, "MAP CACHE", &lv_font_montserrat_14, COLOR_CYAN);
    lv_obj_center(s_map_status);

    map_control(s_sky_plot, "+", 58, map_zoom_event, (void *)(intptr_t)1);
    map_control(s_sky_plot, "-", 108, map_zoom_event, (void *)(intptr_t)-1);
    map_control(s_sky_plot, LV_SYMBOL_HOME, 158, map_home_event, NULL);
    s_map_zoom_label = label(s_sky_plot, "Z8", &lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_set_pos(s_map_zoom_label, 432, 206);

    lv_obj_t *attribution_bg = lv_obj_create(s_sky_plot);
    lv_obj_align(attribution_bg, LV_ALIGN_BOTTOM_RIGHT, -7, -6);
    lv_obj_set_size(attribution_bg, 158, 18);
    lv_obj_set_style_radius(attribution_bg, 10, 0);
    lv_obj_set_style_bg_color(attribution_bg, lv_color_hex(0x07101b), 0);
    lv_obj_set_style_bg_opa(attribution_bg, LV_OPA_80, 0);
    lv_obj_set_style_border_width(attribution_bg, 0, 0);
    lv_obj_set_style_pad_all(attribution_bg, 0, 0);
    lv_obj_remove_flag(attribution_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *attribution = label(attribution_bg, "(c) OpenStreetMap contributors", &lv_font_montserrat_10, COLOR_MUTED);
    lv_obj_center(attribution);

    sky_status_overlay(page);

    s_flight_detail = panel(page, 10, 558, 460, 182, 0x0a111e);
    lv_obj_set_style_bg_opa(s_flight_detail, LV_OPA_90, 0);
    lv_obj_add_flag(s_flight_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *eyebrow = label(s_flight_detail, "SELECTED FLIGHT", &lv_font_montserrat_14, COLOR_PURPLE);
    lv_obj_set_pos(eyebrow, 14, 10);
    s_plane_call = label(s_flight_detail, "--", &lv_font_montserrat_24, COLOR_TEXT);
    lv_obj_set_pos(s_plane_call, 14, 31);
    s_plane_type = label(s_flight_detail, "--", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(s_plane_type, 128, 37);
    s_plane_route = label(s_flight_detail, "TAP AIRCRAFT FOR ROUTE", &lv_font_montserrat_20, COLOR_YELLOW);
    lv_obj_set_pos(s_plane_route, 14, 69);
    s_plane_route_names = label(s_flight_detail, "", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(s_plane_route_names, 15, 96);
    lv_obj_t *duration_cap = label(s_flight_detail, "EST. DURATION", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(duration_cap, 326, 52);
    s_plane_duration = label(s_flight_detail, "--", &lv_font_montserrat_18, COLOR_GREEN);
    lv_obj_set_pos(s_plane_duration, 342, 76);
    s_plane_distance = label(s_flight_detail, "-- nm / -- deg", &lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_set_pos(s_plane_distance, 15, 148);
    lv_obj_t *alt_cap = label(s_flight_detail, "ALTITUDE", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(alt_cap, 190, 121);
    s_plane_altitude = label(s_flight_detail, "-- ft", &lv_font_montserrat_18, COLOR_CYAN);
    lv_obj_set_pos(s_plane_altitude, 190, 144);
    lv_obj_t *speed_cap = label(s_flight_detail, "SPEED", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(speed_cap, 330, 121);
    s_plane_speed = label(s_flight_detail, "-- kt", &lv_font_montserrat_18, COLOR_GREEN);
    lv_obj_set_pos(s_plane_speed, 330, 144);

    s_map_center_lat = s_settings->latitude;
    s_map_center_lon = s_settings->longitude;
    s_map_zoom = s_settings->map_zoom;
    request_map_tiles();
    s_pages[PAGE_SKY] = page;
}

static void create_orbit_page(void)
{
    lv_obj_t *page = lv_obj_create(s_track);
    lv_obj_set_pos(page, 480, 0);
    lv_obj_set_size(page, 480, 800);
    plain(page);
    lv_obj_add_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    header(page);
    lv_obj_t *title = label(page, "Objects in orbit", &lv_font_montserrat_28, COLOR_TEXT);
    lv_obj_set_pos(title, 18, 61);
    s_catalog_count = label(page, "CELESTRAK CATALOG", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(s_catalog_count, 20, 93);

    lv_obj_t *space = panel(page, 16, 120, 448, 438, 0x060a16);
    for (int diameter = 210; diameter <= 410; diameter += 100) {
        lv_obj_t *orbit = lv_obj_create(space);
        lv_obj_set_size(orbit, diameter, diameter / 2);
        lv_obj_align(orbit, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(orbit, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(orbit, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(orbit, 1, 0);
        lv_obj_set_style_border_color(orbit, lv_color_hex(0x2c325b), 0);
        lv_obj_set_style_border_opa(orbit, LV_OPA_60, 0);
        lv_obj_set_style_transform_rotation(orbit, (diameter - 210) * 2, 0);
    }
    lv_obj_t *glow = lv_obj_create(space);
    lv_obj_set_size(glow, 226, 226);
    lv_obj_center(glow);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(glow, lv_color_hex(0x175b7e), 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_30, 0);
    lv_obj_set_style_border_width(glow, 18, 0);
    lv_obj_set_style_border_color(glow, lv_color_hex(0x102f58), 0);
    lv_obj_set_style_border_opa(glow, LV_OPA_70, 0);
    lv_obj_t *earth = lv_obj_create(space);
    lv_obj_set_size(earth, 176, 176);
    lv_obj_center(earth);
    lv_obj_set_style_radius(earth, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(earth, lv_color_hex(0x0d4b75), 0);
    lv_obj_set_style_bg_grad_color(earth, lv_color_hex(0x172f55), 0);
    lv_obj_set_style_bg_grad_dir(earth, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(earth, 2, 0);
    lv_obj_set_style_border_color(earth, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_border_opa(earth, LV_OPA_50, 0);
    lv_obj_t *earth_name = label(earth, "EARTH", &lv_font_montserrat_18, COLOR_TEXT);
    lv_obj_center(earth_name);
    for (int i = 0; i < ORB_MAX_SATELLITES; i++) {
        s_orbit_markers[i] = lv_obj_create(space);
        lv_obj_set_size(s_orbit_markers[i], i == 0 ? 12 : 8, i == 0 ? 12 : 8);
        lv_obj_set_style_radius(s_orbit_markers[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_orbit_markers[i], lv_color_hex(i == 0 ? COLOR_ORANGE : COLOR_PURPLE), 0);
        lv_obj_set_style_border_width(s_orbit_markers[i], 2, 0);
        lv_obj_set_style_border_color(s_orbit_markers[i], lv_color_hex(COLOR_TEXT), 0);
        lv_obj_add_flag(s_orbit_markers[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *list = panel(page, 16, 574, 448, 164, COLOR_PANEL);
    lv_obj_t *list_title = label(list, "LIVE CATALOG", &lv_font_montserrat_14, COLOR_GREEN);
    lv_obj_set_pos(list_title, 16, 16);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *dot = lv_obj_create(list);
        const int column = i % 2;
        const int row = i / 2;
        lv_obj_set_pos(dot, 16 + column * 216, 55 + row * 52);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(i == 0 ? COLOR_ORANGE : COLOR_PURPLE), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        s_orbit_names[i] = label(list, "--", &lv_font_montserrat_16, COLOR_TEXT);
        lv_obj_set_pos(s_orbit_names[i], 34 + column * 216, 45 + row * 52);
        s_orbit_positions[i] = label(list, "--", &lv_font_montserrat_14, COLOR_MUTED);
        lv_obj_set_pos(s_orbit_positions[i], 34 + column * 216, 69 + row * 52);
    }
    lv_obj_t *note = label(list, "Locally propagated from fresh orbit data", &lv_font_montserrat_14, 0x64728b);
    lv_obj_set_pos(note, 16, 138);
    s_pages[PAGE_ORBIT] = page;
}

static void create_iss_page(void)
{
    lv_obj_t *page = lv_obj_create(s_track);
    lv_obj_set_pos(page, 960, 0);
    lv_obj_set_size(page, 480, 800);
    plain(page);
    lv_obj_add_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    header(page);
    lv_obj_t *title = label(page, "International Space Station", &lv_font_montserrat_28, COLOR_TEXT);
    lv_obj_set_pos(title, 18, 61);
    s_iss_map_plot = panel(page, 16, 120, ISS_MAP_WIDTH, ISS_MAP_HEIGHT, 0x08121e);
    for (int i = 0; i < MAP_TILE_SLOTS; i++) {
        s_iss_map_tiles[i].image = lv_image_create(s_iss_map_plot);
        lv_obj_add_flag(s_iss_map_tiles[i].image, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 14; i++) {
        s_iss_trail[i] = lv_obj_create(s_iss_map_plot);
        lv_obj_set_size(s_iss_trail[i], 5, 5);
        lv_obj_set_style_radius(s_iss_trail[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_iss_trail[i], lv_color_hex(COLOR_CYAN), 0);
        lv_obj_set_style_bg_opa(s_iss_trail[i], 40 + i * 12, 0);
        lv_obj_set_style_border_width(s_iss_trail[i], 0, 0);
        lv_obj_add_flag(s_iss_trail[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_iss_halo = lv_obj_create(s_iss_map_plot);
    lv_obj_set_size(s_iss_halo, 34, 34);
    lv_obj_set_style_radius(s_iss_halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_iss_halo, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_set_style_bg_opa(s_iss_halo, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_iss_halo, 1, 0);
    lv_obj_set_style_border_color(s_iss_halo, lv_color_hex(COLOR_ORANGE), 0);
    s_iss_marker = lv_obj_create(s_iss_map_plot);
    lv_obj_set_size(s_iss_marker, 14, 14);
    lv_obj_set_style_radius(s_iss_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_iss_marker, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_set_style_border_width(s_iss_marker, 3, 0);
    lv_obj_set_style_border_color(s_iss_marker, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_t *map_label = label(s_iss_map_plot, "LIVE GROUND TRACK", &lv_font_montserrat_14, COLOR_CYAN);
    lv_obj_set_pos(map_label, 12, 12);

    lv_obj_t *attribution_bg = lv_obj_create(s_iss_map_plot);
    lv_obj_align(attribution_bg, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
    lv_obj_set_size(attribution_bg, 158, 18);
    lv_obj_set_style_radius(attribution_bg, 9, 0);
    lv_obj_set_style_bg_color(attribution_bg, lv_color_hex(0x07101b), 0);
    lv_obj_set_style_bg_opa(attribution_bg, LV_OPA_80, 0);
    lv_obj_set_style_border_width(attribution_bg, 0, 0);
    lv_obj_set_style_pad_all(attribution_bg, 0, 0);
    lv_obj_remove_flag(attribution_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *attribution = label(attribution_bg, "(c) OpenStreetMap contributors",
                                  &lv_font_montserrat_10, COLOR_MUTED);
    lv_obj_center(attribution);

    lv_obj_t *detail = panel(page, 16, 574, 448, 164, COLOR_PANEL);
    lv_obj_t *eyebrow = label(detail, "ISS / NORAD 25544", &lv_font_montserrat_14, COLOR_ORANGE);
    lv_obj_set_pos(eyebrow, 15, 13);
    s_iss_latlon = label(detail, "--", &lv_font_montserrat_18, COLOR_TEXT);
    lv_obj_set_pos(s_iss_latlon, 15, 38);
    lv_obj_t *alt_cap = label(detail, "ALTITUDE", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(alt_cap, 186, 17);
    s_iss_altitude = label(detail, "-- km", &lv_font_montserrat_20, COLOR_CYAN);
    lv_obj_set_pos(s_iss_altitude, 186, 40);
    lv_obj_t *vel_cap = label(detail, "VELOCITY", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(vel_cap, 310, 17);
    s_iss_velocity = label(detail, "-- km/h", &lv_font_montserrat_20, COLOR_GREEN);
    lv_obj_set_pos(s_iss_velocity, 310, 40);
    s_iss_visibility = label(detail, "--", &lv_font_montserrat_16, COLOR_ORANGE);
    lv_obj_set_pos(s_iss_visibility, 15, 112);
    s_iss_range = label(detail, "--", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(s_iss_range, 186, 94);
    lv_obj_set_width(s_iss_range, 245);
    lv_label_set_long_mode(s_iss_range, LV_LABEL_LONG_WRAP);
    s_pages[PAGE_ISS] = page;
}

static void config_dialog_close(void)
{
    if (!s_config_dialog) return;
    lv_obj_delete(s_config_dialog);
    s_config_dialog = NULL;
    s_config_keyboard = NULL;
}

static void config_cancel(lv_event_t *event) { (void)event; config_dialog_close(); }

static void field_focus(lv_event_t *event)
{
    if (!s_config_keyboard) return;
    lv_obj_t *field = lv_event_get_target(event);
    lv_keyboard_set_textarea(s_config_keyboard, field);
    const bool numeric = field == s_field_lat || field == s_field_lon;
    lv_keyboard_set_mode(s_config_keyboard, numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_flag(s_config_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_config_keyboard);
}

static void keyboard_event(lv_event_t *event)
{
    (void)event;
    if (!s_config_keyboard) return;
    lv_keyboard_set_textarea(s_config_keyboard, NULL);
    lv_obj_add_flag(s_config_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void config_save(lv_event_t *event)
{
    (void)event;
    const double latitude = strtod(lv_textarea_get_text(s_field_lat), NULL);
    const double longitude = strtod(lv_textarea_get_text(s_field_lon), NULL);
    const char *city = lv_textarea_get_text(s_field_city);
    const char *ssid = lv_textarea_get_text(s_field_ssid);
    const char *password = lv_textarea_get_text(s_field_password);
    if (!city[0] && (latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180)) return;
    const bool wifi_changed = strcmp(s_settings->wifi_ssid, ssid) != 0 ||
                              strcmp(s_settings->wifi_password, password) != 0;
    orb_wifi_status_t wifi;
    orb_wifi_get_status(&wifi);
    strlcpy(s_settings->wifi_ssid, ssid, sizeof(s_settings->wifi_ssid));
    strlcpy(s_settings->wifi_password, password, sizeof(s_settings->wifi_password));
    if (s_settings->wifi_ssid[0] && (wifi_changed || !wifi.connected)) {
        orb_wifi_connect(s_settings->wifi_ssid, s_settings->wifi_password);
    }
    if (city[0]) {
        s_geocode_pending = orb_geocode_request(city);
        strlcpy(s_geocode_message, s_geocode_pending ? "Finding city..." : "City lookup unavailable",
                sizeof(s_geocode_message));
        s_geocode_message_until = esp_timer_get_time() / 1000 + 10000;
    } else {
        s_settings->city[0] = '\0';
        s_settings->latitude = latitude;
        s_settings->longitude = longitude;
        s_map_center_lat = latitude;
        s_map_center_lon = longitude;
        for (int i = 0; i < MAP_TILE_SLOTS; i++) release_map_slot(&s_map_tiles[i]);
        position_map_tiles();
        request_map_tiles();
        s_timezone_request_ms = 0;
    }
    orb_settings_save(s_settings);
    config_dialog_close();
}

static lv_obj_t *textarea(lv_obj_t *parent, const char *placeholder, int x, int y, int width)
{
    lv_obj_t *field = lv_textarea_create(parent);
    lv_obj_set_pos(field, x, y);
    lv_obj_set_size(field, width, 48);
    lv_textarea_set_one_line(field, true);
    lv_textarea_set_placeholder_text(field, placeholder);
    lv_obj_set_style_radius(field, 13, 0);
    lv_obj_set_style_bg_color(field, lv_color_hex(0x121a2a), 0);
    lv_obj_set_style_border_color(field, lv_color_hex(0x34425b), 0);
    lv_obj_set_style_text_color(field, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(field, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(field, field_focus, LV_EVENT_CLICKED, NULL);
    return field;
}

static void config_open(lv_event_t *event)
{
    (void)event;
    if (s_config_dialog) return;
    s_config_dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_config_dialog, 480, 800);
    lv_obj_set_pos(s_config_dialog, 0, 0);
    lv_obj_set_style_bg_color(s_config_dialog, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_config_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_config_dialog, 0, 0);
    lv_obj_set_style_radius(s_config_dialog, 0, 0);
    lv_obj_remove_flag(s_config_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = label(s_config_dialog, "Connection & observer", &lv_font_montserrat_24, COLOR_TEXT);
    lv_obj_set_pos(title, 20, 18);
    s_field_ssid = textarea(s_config_dialog, "WiFi network", 20, 64, 440);
    s_field_password = textarea(s_config_dialog, "WiFi password", 20, 124, 440);
    s_field_city = textarea(s_config_dialog, "City or town (e.g. Berlin, DE)", 20, 184, 440);
    s_field_lat = textarea(s_config_dialog, "Latitude (manual fallback)", 20, 244, 440);
    s_field_lon = textarea(s_config_dialog, "Longitude (manual fallback)", 20, 304, 440);
    lv_textarea_set_text(s_field_ssid, s_settings->wifi_ssid);
    lv_textarea_set_text(s_field_password, s_settings->wifi_password);
    lv_textarea_set_password_mode(s_field_password, true);
    lv_textarea_set_text(s_field_city, s_settings->city);
    lv_textarea_set_max_length(s_field_city, sizeof(s_settings->city) - 1);
    char number[24];
    snprintf(number, sizeof(number), "%.5f", s_settings->latitude);
    lv_textarea_set_text(s_field_lat, number);
    snprintf(number, sizeof(number), "%.5f", s_settings->longitude);
    lv_textarea_set_text(s_field_lon, number);
    lv_obj_t *cancel = pill(s_config_dialog, "CANCEL", 264, 372, 92, 0x222d41);
    lv_obj_t *save = pill(s_config_dialog, "SAVE", 368, 372, 92, COLOR_PURPLE);
    lv_obj_add_event_cb(cancel, config_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(save, config_save, LV_EVENT_CLICKED, NULL);
    s_config_keyboard = lv_keyboard_create(s_config_dialog);
    lv_obj_set_size(s_config_keyboard, 480, 360);
    lv_obj_align(s_config_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_config_keyboard, keyboard_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_config_keyboard, keyboard_event, LV_EVENT_CANCEL, NULL);
    lv_keyboard_set_textarea(s_config_keyboard, NULL);
    lv_obj_add_flag(s_config_keyboard, LV_OBJ_FLAG_HIDDEN);
}

enum { UNIT_GROUP_DISTANCE, UNIT_GROUP_ALTITUDE, UNIT_GROUP_SPEED };

static uint8_t unit_value(int group)
{
    if (group == UNIT_GROUP_ALTITUDE) return s_settings->altitude_unit;
    if (group == UNIT_GROUP_SPEED) return s_settings->speed_unit;
    return s_settings->distance_unit;
}

static void refresh_unit_buttons(void)
{
    static const int counts[] = {3, 2, 3};
    for (int group = 0; group < 3; group++) {
        for (int value = 0; value < counts[group]; value++) {
            lv_obj_t *button = s_unit_buttons[group][value];
            if (!button) continue;
            const bool selected = value == unit_value(group);
            lv_obj_set_style_bg_color(button, lv_color_hex(selected ? 0x16728a : 0x182235), 0);
            lv_obj_set_style_border_color(button, lv_color_hex(selected ? COLOR_CYAN : 0x34425b), 0);
        }
    }
}

static void unit_event(lv_event_t *event)
{
    const intptr_t encoded = (intptr_t)lv_event_get_user_data(event);
    const int group = (int)((encoded >> 8) & 0xff);
    const uint8_t value = (uint8_t)(encoded & 0xff);
    if (group == UNIT_GROUP_ALTITUDE) s_settings->altitude_unit = value;
    else if (group == UNIT_GROUP_SPEED) s_settings->speed_unit = value;
    else s_settings->distance_unit = value;
    orb_settings_save(s_settings);
    refresh_unit_buttons();
}

static lv_obj_t *unit_choice(lv_obj_t *parent, const char *text, int x, int y, int width, int group, int value)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 36);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_t *caption = label(button, text, &lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_center(caption);
    lv_obj_add_event_cb(button, unit_event, LV_EVENT_CLICKED,
                        (void *)(intptr_t)((group << 8) | value));
    s_unit_buttons[group][value] = button;
    return button;
}

static void refresh_click(lv_event_t *event) { (void)event; orb_data_refresh_now(); }
static void settings_close_event(lv_event_t *event) { (void)event; settings_close(); }

void orb_ui_open_settings(void)
{
    if (s_settings_overlay) return;
    s_settings_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_settings_overlay, 0, -800);
    lv_obj_set_size(s_settings_overlay, 480, 800);
    lv_obj_set_style_bg_color(s_settings_overlay, lv_color_hex(0x070b15), 0);
    lv_obj_set_style_bg_opa(s_settings_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_settings_overlay, 0, 0);
    lv_obj_set_style_radius(s_settings_overlay, 0, 0);
    lv_obj_remove_flag(s_settings_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_settings_overlay, gesture_event, LV_EVENT_GESTURE, NULL);
    lv_obj_t *title = label(s_settings_overlay, "Orb settings", &lv_font_montserrat_28, COLOR_TEXT);
    lv_obj_set_pos(title, 18, 20);
    lv_obj_t *hint = label(s_settings_overlay, "WiFi powers live feeds. Your location stays on the device.", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(hint, 19, 58);
    lv_obj_set_width(hint, 340);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_t *close = pill(s_settings_overlay, "CLOSE", 374, 20, 88, 0x212b3e);
    lv_obj_add_event_cb(close, settings_close_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *network = panel(s_settings_overlay, 16, 112, 448, 210, COLOR_PANEL);
    lv_obj_t *network_title = label(network, "CONNECTION", &lv_font_montserrat_14, COLOR_PURPLE);
    lv_obj_set_pos(network_title, 18, 16);
    s_settings_wifi = label(network, "Not configured", &lv_font_montserrat_20, COLOR_TEXT);
    lv_obj_set_pos(s_settings_wifi, 18, 48);
    s_settings_location = label(network, "--", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(s_settings_location, 18, 81);
    lv_obj_set_width(s_settings_location, 410);
    lv_label_set_long_mode(s_settings_location, LV_LABEL_LONG_DOT);
    lv_obj_t *configure = pill(network, "CONFIGURE", 18, 157, 132, COLOR_PURPLE);
    lv_obj_add_event_cb(configure, config_open, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh = pill(network, "REFRESH NOW", 164, 157, 142, 0x1d654f);
    lv_obj_add_event_cb(refresh, refresh_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *units = panel(s_settings_overlay, 16, 340, 448, 230, COLOR_PANEL);
    lv_obj_t *units_title = label(units, "UNITS", &lv_font_montserrat_14, COLOR_CYAN);
    lv_obj_set_pos(units_title, 18, 14);
    lv_obj_t *distance_title = label(units, "Distance", &lv_font_montserrat_16, COLOR_TEXT);
    lv_obj_set_pos(distance_title, 18, 54);
    unit_choice(units, "nm", 176, 43, 72, UNIT_GROUP_DISTANCE, ORB_DISTANCE_NAUTICAL_MILES);
    unit_choice(units, "km", 256, 43, 72, UNIT_GROUP_DISTANCE, ORB_DISTANCE_KILOMETRES);
    unit_choice(units, "mi", 336, 43, 72, UNIT_GROUP_DISTANCE, ORB_DISTANCE_MILES);
    lv_obj_t *altitude_title = label(units, "Altitude", &lv_font_montserrat_16, COLOR_TEXT);
    lv_obj_set_pos(altitude_title, 18, 107);
    unit_choice(units, "ft", 256, 96, 72, UNIT_GROUP_ALTITUDE, ORB_ALTITUDE_FEET);
    unit_choice(units, "m", 336, 96, 72, UNIT_GROUP_ALTITUDE, ORB_ALTITUDE_METRES);
    lv_obj_t *speed_title = label(units, "Speed", &lv_font_montserrat_16, COLOR_TEXT);
    lv_obj_set_pos(speed_title, 18, 160);
    unit_choice(units, "kt", 176, 149, 72, UNIT_GROUP_SPEED, ORB_SPEED_KNOTS);
    unit_choice(units, "km/h", 256, 149, 72, UNIT_GROUP_SPEED, ORB_SPEED_KMH);
    unit_choice(units, "mph", 336, 149, 72, UNIT_GROUP_SPEED, ORB_SPEED_MPH);
    refresh_unit_buttons();

    lv_obj_t *sources = panel(s_settings_overlay, 16, 588, 448, 148, 0x0a111e);
    lv_obj_t *source_title = label(sources, "LIVE SOURCES", &lv_font_montserrat_14, COLOR_GREEN);
    lv_obj_set_pos(source_title, 18, 13);
    lv_obj_t *source_text = label(sources,
        "Aircraft ADSB.lol  /  Routes ADSBDB\nISS WhereTheISS  /  Orbit CelesTrak\nPlaces Nominatim  /  Time TimeAPI.io",
        &lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_set_pos(source_text, 18, 39);
    lv_obj_t *source_note = label(sources, "Coverage follows the visible map. Swipe up to close.",
                                  &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(source_note, 18, 108);
    lv_obj_set_width(source_note, 410);
    lv_label_set_long_mode(source_note, LV_LABEL_LONG_WRAP);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_settings_overlay);
    lv_anim_set_exec_cb(&animation, animate_y);
    lv_anim_set_values(&animation, -800, 0);
    lv_anim_set_duration(&animation, 320);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

static double longitude_delta(double target, double current)
{
    double delta = target - current;
    while (delta > 180) delta -= 360;
    while (delta < -180) delta += 360;
    return delta;
}

static void refresh_ui(lv_timer_t *timer)
{
    (void)timer;
    const int64_t refresh_ms = esp_timer_get_time() / 1000;
    orb_geocode_result_t location;
    while (orb_geocode_take_result(&location)) {
        s_geocode_pending = false;
        strlcpy(s_geocode_message, location.message, sizeof(s_geocode_message));
        s_geocode_message_until = esp_timer_get_time() / 1000 + 6000;
        if (location.success) {
            strlcpy(s_settings->city, location.city, sizeof(s_settings->city));
            s_settings->latitude = location.latitude;
            s_settings->longitude = location.longitude;
            if (location.timezone[0]) {
                strlcpy(s_settings->timezone, location.timezone, sizeof(s_settings->timezone));
                s_settings->utc_offset_seconds = location.utc_offset_seconds;
            }
            orb_settings_save(s_settings);
            s_map_center_lat = location.latitude;
            s_map_center_lon = location.longitude;
            for (int i = 0; i < MAP_TILE_SLOTS; i++) release_map_slot(&s_map_tiles[i]);
            position_map_tiles();
            request_map_tiles();
            s_timezone_request_ms = 0;
        }
    }
    orb_timezone_result_t timezone;
    while (orb_timezone_take_result(&timezone)) {
        if (timezone.success && fabs(timezone.latitude - s_settings->latitude) < 0.01 &&
            fabs(timezone.longitude - s_settings->longitude) < 0.01) {
            strlcpy(s_settings->timezone, timezone.timezone, sizeof(s_settings->timezone));
            s_settings->utc_offset_seconds = timezone.utc_offset_seconds;
            orb_settings_save(s_settings);
        } else if (!timezone.success) {
            s_timezone_request_ms = refresh_ms - TIMEZONE_REFRESH_MS + 60000;
        }
    }
    orb_data_snapshot_t data;
    orb_data_get_snapshot(&data);
    orb_wifi_status_t wifi;
    orb_wifi_get_status(&wifi);
    if (wifi.connected &&
        (s_timezone_request_ms == 0 || refresh_ms - s_timezone_request_ms >= TIMEZONE_REFRESH_MS) &&
        orb_timezone_request(s_settings->latitude, s_settings->longitude)) {
        s_timezone_request_ms = refresh_ms;
    }
    const bool live = wifi.connected && (data.aircraft_live || data.iss_live);
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_set_style_bg_color(s_live_dot[i], lv_color_hex(live ? COLOR_GREEN : COLOR_ORANGE), 0);
        lv_label_set_text(s_live_text[i], live ? "LIVE" : (wifi.connected ? "SYNC" : "DEMO"));
        lv_obj_set_style_text_color(s_live_text[i], lv_color_hex(live ? COLOR_GREEN : COLOR_ORANGE), 0);
    }
    char text[128];
    time_t now = time(NULL);
    if (now >= 1609459200) {
        struct tm local;
        const time_t location_time = now + s_settings->utc_offset_seconds;
        gmtime_r(&location_time, &local);
        strftime(text, sizeof(text), "%H:%M", &local);
    } else strlcpy(text, "--:--", sizeof(text));
    for (int i = 0; i < PAGE_COUNT; i++) lv_label_set_text(s_clock[i], text);

    char distance_text[32];
    format_distance(distance_text, sizeof(distance_text), s_map_radius_nm, 0);
    snprintf(text, sizeof(text), "%u AIRCRAFT  /  VIEW %s", (unsigned)data.aircraft_count, distance_text);
    if (s_aircraft_count) lv_label_set_text(s_aircraft_count, text);

    orb_route_result_t route;
    while (orb_route_take_result(&route)) {
        if (strcmp(route.callsign, s_route_callsign) != 0) continue;
        if (route.success) {
            snprintf(text, sizeof(text), "%s  >  %s", route.origin_code, route.destination_code);
            lv_label_set_text(s_plane_route, text);
            snprintf(text, sizeof(text), "%s  to  %s", route.origin_city, route.destination_city);
            lv_label_set_text(s_plane_route_names, text);
            format_duration(text, sizeof(text), route.estimated_minutes);
            lv_label_set_text(s_plane_duration, text);
        } else {
            const bool no_schedule = strcmp(route.message, "No scheduled route") == 0;
            lv_label_set_text(s_plane_route, no_schedule ? "NO SCHEDULED ROUTE" : "ROUTE LOOKUP OFFLINE");
            lv_label_set_text(s_plane_route_names,
                              no_schedule ? "Private, local or unscheduled flight" : "Tap aircraft to retry");
            lv_label_set_text(s_plane_duration, "--");
        }
    }
    orb_map_tile_t map_tile;
    while (orb_map_take_tile(&map_tile)) {
        if (map_tile.generation == s_iss_map_generation) accept_iss_map_tile(&map_tile);
        else accept_map_tile(&map_tile);
    }
    orb_map_status_t map_status;
    orb_map_get_status(&map_status);
    lv_label_set_text(s_map_status, map_status.message);
    lv_obj_set_style_text_color(s_map_status, lv_color_hex(map_status.loading ? COLOR_ORANGE :
                                (map_status.sd_mounted ? COLOR_CYAN : COLOR_ORANGE)), 0);
    if (!map_status.loading && map_status.sd_mounted) lv_obj_add_flag(s_map_status_pill, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_map_status_pill, LV_OBJ_FLAG_HIDDEN);

    const double map_world = map_world_size(s_map_zoom);
    const double map_center_x = map_lon_to_world_x(s_map_center_lon, s_map_zoom);
    const double map_center_y = map_lat_to_world_y(s_map_center_lat, s_map_zoom);
    const double observer_x = MAP_WIDTH * 0.5 + map_wrapped_delta(
        map_lon_to_world_x(s_settings->longitude, s_map_zoom), map_center_x, map_world);
    const double observer_y = MAP_HEIGHT * 0.5 + map_lat_to_world_y(s_settings->latitude, s_map_zoom) - map_center_y;
    if (observer_x >= -10 && observer_x <= MAP_WIDTH + 10 && observer_y >= -10 && observer_y <= MAP_HEIGHT + 10) {
        lv_obj_set_pos(s_observer_marker, (int)observer_x - 9, (int)observer_y - 9);
        lv_obj_remove_flag(s_observer_marker, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_observer_marker, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_selected_aircraft >= (int)data.aircraft_count) {
        s_selected_aircraft = -1;
        s_route_callsign[0] = '\0';
        lv_obj_add_flag(s_flight_detail, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < ORB_MAX_AIRCRAFT; i++) {
        if (i >= (int)data.aircraft_count) {
            lv_obj_add_flag(s_aircraft_markers[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_aircraft_names[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const orb_aircraft_t *plane = &data.aircraft[i];
        const double x = MAP_WIDTH * 0.5 + map_wrapped_delta(
            map_lon_to_world_x(plane->longitude, s_map_zoom), map_center_x, map_world);
        const double y = MAP_HEIGHT * 0.5 + map_lat_to_world_y(plane->latitude, s_map_zoom) - map_center_y;
        if (x < -20 || x > MAP_WIDTH + 20 || y < -20 || y > MAP_HEIGHT + 20) {
            lv_obj_add_flag(s_aircraft_markers[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_aircraft_names[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_set_pos(s_aircraft_markers[i], (int)x - 19, (int)y - 19);
        lv_obj_set_style_transform_rotation(s_aircraft_markers[i], (int)(plane->track_deg * 10), 0);
        lv_obj_set_style_opa(s_aircraft_markers[i], i == s_selected_aircraft ? LV_OPA_COVER : LV_OPA_90, 0);
        const uint32_t aircraft_color = i == s_selected_aircraft ? COLOR_GREEN : COLOR_YELLOW;
        lv_obj_set_style_line_color(s_aircraft_outlines[i], lv_color_hex(aircraft_color), 0);
        lv_obj_set_style_line_color(s_aircraft_spines[i], lv_color_hex(aircraft_color), 0);
        lv_label_set_text(s_aircraft_names[i], plane->callsign);
        lv_obj_set_style_text_color(s_aircraft_names[i],
                                    lv_color_hex(i == s_selected_aircraft ? COLOR_GREEN : COLOR_YELLOW), 0);
        lv_obj_set_pos(s_aircraft_names[i], (int)x + 13, (int)y - 9);
        lv_obj_remove_flag(s_aircraft_markers[i], LV_OBJ_FLAG_HIDDEN);
        if (i == s_selected_aircraft) lv_obj_remove_flag(s_aircraft_names[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_aircraft_names[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (data.aircraft_count && s_selected_aircraft >= 0) {
        const orb_aircraft_t *plane = &data.aircraft[s_selected_aircraft];
        lv_label_set_text(s_plane_call, plane->callsign);
        if (s_route_callsign[0] && strcmp(s_route_callsign, plane->callsign) != 0) {
            s_route_callsign[0] = '\0';
            lv_label_set_text(s_plane_route, "TAP AIRCRAFT FOR ROUTE");
            lv_label_set_text(s_plane_route_names, "");
            lv_label_set_text(s_plane_duration, "--");
        }
        snprintf(text, sizeof(text), "%s  /  %s", plane->type, plane->registration);
        lv_label_set_text(s_plane_type, text);
        if (plane->on_ground) strlcpy(text, "GROUND", sizeof(text));
        else format_altitude(text, sizeof(text), plane->altitude_ft);
        lv_label_set_text(s_plane_altitude, text);
        format_speed(text, sizeof(text), plane->speed_knots);
        lv_label_set_text(s_plane_speed, text);
        format_distance(distance_text, sizeof(distance_text), plane->distance_nm, 1);
        snprintf(text, sizeof(text), "%s  /  %03.0f deg", distance_text, plane->bearing_deg);
        lv_label_set_text(s_plane_distance, text);
    }

    snprintf(text, sizeof(text), "%u OBJECTS  /  %s", (unsigned)data.satellite_count, data.satellites_live ? "LIVE ELEMENTS" : "DEMO ORBITS");
    lv_label_set_text(s_catalog_count, text);
    const double center_lon = fmod(esp_timer_get_time() / 1000000.0 * 1.2, 360.0) - 180.0;
    for (int i = 0; i < ORB_MAX_SATELLITES; i++) {
        if (i >= (int)data.satellite_count) {
            lv_obj_add_flag(s_orbit_markers[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const orb_satellite_t *sat = &data.satellites[i];
        const double lat = sat->latitude * M_PI / 180.0;
        const double dlon = longitude_delta(sat->longitude, center_lon) * M_PI / 180.0;
        const double visible = cos(lat) * cos(dlon);
        const int x = 224 + (int)(cos(lat) * sin(dlon) * 112);
        const int y = 219 - (int)(sin(lat) * 96);
        lv_obj_set_pos(s_orbit_markers[i], x - 5, y - 5);
        lv_obj_set_style_bg_opa(s_orbit_markers[i], visible >= 0 ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_remove_flag(s_orbit_markers[i], LV_OBJ_FLAG_HIDDEN);
        if (i < 4) {
            lv_label_set_text(s_orbit_names[i], sat->name);
            snprintf(text, sizeof(text), "%+.1f / %+.1f", sat->latitude, sat->longitude);
            lv_label_set_text(s_orbit_positions[i], text);
        }
    }

    if (s_display_iss_lat == 0 && s_display_iss_lon == 0) {
        s_display_iss_lat = data.iss_latitude;
        s_display_iss_lon = data.iss_longitude;
    }
    s_display_iss_lat += (data.iss_latitude - s_display_iss_lat) * 0.12;
    s_display_iss_lon += longitude_delta(data.iss_longitude, s_display_iss_lon) * 0.12;
    if (s_display_iss_lon > 180) s_display_iss_lon -= 360;
    if (s_display_iss_lon < -180) s_display_iss_lon += 360;
    const double iss_world = map_world_size(ISS_MAP_ZOOM);
    double iss_center_x = map_lon_to_world_x(s_iss_map_center_lon, ISS_MAP_ZOOM);
    double iss_center_y = map_lat_to_world_y(s_iss_map_center_lat, ISS_MAP_ZOOM);
    int iss_x = (int)lround(ISS_MAP_WIDTH * 0.5 + map_wrapped_delta(
        map_lon_to_world_x(s_display_iss_lon, ISS_MAP_ZOOM), iss_center_x, iss_world));
    int iss_y = (int)lround(ISS_MAP_HEIGHT * 0.5 +
        map_lat_to_world_y(s_display_iss_lat, ISS_MAP_ZOOM) - iss_center_y);
    if (s_page == PAGE_ISS && s_iss_map_generation &&
        (iss_x < 80 || iss_x > ISS_MAP_WIDTH - 80 ||
         iss_y < 70 || iss_y > ISS_MAP_HEIGHT - 70) &&
        refresh_ms - s_last_iss_map_request_ms > 10000) {
        request_iss_map_tiles();
        iss_center_x = map_lon_to_world_x(s_iss_map_center_lon, ISS_MAP_ZOOM);
        iss_center_y = map_lat_to_world_y(s_iss_map_center_lat, ISS_MAP_ZOOM);
        iss_x = (int)lround(ISS_MAP_WIDTH * 0.5 + map_wrapped_delta(
            map_lon_to_world_x(s_display_iss_lon, ISS_MAP_ZOOM), iss_center_x, iss_world));
        iss_y = (int)lround(ISS_MAP_HEIGHT * 0.5 +
            map_lat_to_world_y(s_display_iss_lat, ISS_MAP_ZOOM) - iss_center_y);
    }
    lv_obj_set_pos(s_iss_marker, iss_x - 7, iss_y - 7);
    lv_obj_set_pos(s_iss_halo, iss_x - 17, iss_y - 17);
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_trail_ms > 3500) {
        const int trail_index = s_iss_trail_head++ % 14;
        s_iss_trail_lat[trail_index] = s_display_iss_lat;
        s_iss_trail_lon[trail_index] = s_display_iss_lon;
        s_iss_trail_valid[trail_index] = true;
        s_last_trail_ms = now_ms;
    }
    for (int i = 0; i < 14; i++) {
        if (!s_iss_trail_valid[i]) continue;
        const int trail_x = (int)lround(ISS_MAP_WIDTH * 0.5 + map_wrapped_delta(
            map_lon_to_world_x(s_iss_trail_lon[i], ISS_MAP_ZOOM), iss_center_x, iss_world));
        const int trail_y = (int)lround(ISS_MAP_HEIGHT * 0.5 +
            map_lat_to_world_y(s_iss_trail_lat[i], ISS_MAP_ZOOM) - iss_center_y);
        if (trail_x < -5 || trail_x > ISS_MAP_WIDTH + 5 ||
            trail_y < -5 || trail_y > ISS_MAP_HEIGHT + 5) {
            lv_obj_add_flag(s_iss_trail[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_pos(s_iss_trail[i], trail_x - 2, trail_y - 2);
            lv_obj_remove_flag(s_iss_trail[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    snprintf(text, sizeof(text), "%+.2f / %+.2f", data.iss_latitude, data.iss_longitude);
    lv_label_set_text(s_iss_latlon, text);
    if (s_settings->altitude_unit == ORB_ALTITUDE_METRES) snprintf(text, sizeof(text), "%.0f km", data.iss_altitude_km);
    else snprintf(text, sizeof(text), "%.0f ft", data.iss_altitude_km * 3280.8399);
    lv_label_set_text(s_iss_altitude, text);
    format_speed(text, sizeof(text), data.iss_velocity_kmh / 1.852);
    lv_label_set_text(s_iss_velocity, text);
    snprintf(text, sizeof(text), "%s / %s", data.iss_live ? "LIVE" : "DEMO", data.iss_visibility);
    lv_label_set_text(s_iss_visibility, text);
    float distance_nm, bearing;
    const double saved_lat = s_settings->latitude, saved_lon = s_settings->longitude;
    const double dlat = (data.iss_latitude - saved_lat) * M_PI / 180.0;
    const double dlon = longitude_delta(data.iss_longitude, saved_lon) * M_PI / 180.0;
    const double a = sin(dlat / 2) * sin(dlat / 2) + cos(saved_lat * M_PI / 180.0) * cos(data.iss_latitude * M_PI / 180.0) * sin(dlon / 2) * sin(dlon / 2);
    const double central = 2 * atan2(sqrt(a), sqrt(1 - a));
    distance_nm = (float)(3440.065 * central);
    bearing = (float)(central * 180.0 / M_PI);
    format_distance(distance_text, sizeof(distance_text), distance_nm, 0);
    snprintf(text, sizeof(text), "%s from your location\n%s", distance_text,
             bearing < 20.0 ? "Above your horizon" : "Below your horizon");
    lv_label_set_text(s_iss_range, text);

    if (s_settings_overlay) {
        lv_label_set_text(s_settings_wifi, wifi.connected ? "WiFi connected" : (s_settings->wifi_ssid[0] ? "Connecting..." : "Not configured"));
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (s_geocode_pending || now_ms < s_geocode_message_until) {
            strlcpy(text, s_geocode_message, sizeof(text));
        } else if (s_settings->city[0]) {
            snprintf(text, sizeof(text), "%.63s  /  %.39s", s_settings->city, s_settings->timezone);
        } else {
            snprintf(text, sizeof(text), "Observer  %.4f, %.4f", s_settings->latitude, s_settings->longitude);
        }
        lv_label_set_text(s_settings_location, text);
    }
}

esp_err_t orb_ui_create(orb_settings_t *settings)
{
    if (!settings) return ESP_ERR_INVALID_ARG;
    s_settings = settings;
    s_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_root, gesture_event, LV_EVENT_GESTURE, NULL);
    s_track = lv_obj_create(s_root);
    lv_obj_set_pos(s_track, 0, 0);
    lv_obj_set_size(s_track, 1440, 800);
    plain(s_track);
    lv_obj_add_flag(s_track, LV_OBJ_FLAG_GESTURE_BUBBLE);
    create_sky_page();
    create_orbit_page();
    create_iss_page();
    create_page_dots();
    lv_screen_load(s_root);
    lv_timer_create(refresh_ui, 100, NULL);
    refresh_ui(NULL);
    return ESP_OK;
}
