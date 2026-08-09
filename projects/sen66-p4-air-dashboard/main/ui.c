#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "bsp/display.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "nvs.h"
#include "wifi_manager.h"

#define C_BG       lv_color_hex(0x071119)
#define C_PANEL    lv_color_hex(0x101E27)
#define C_PANEL_2  lv_color_hex(0x152731)
#define C_TEXT     lv_color_hex(0xF4F7F5)
#define C_MUTED    lv_color_hex(0x8FA5AE)
#define C_GREEN    lv_color_hex(0x62E6A7)
#define C_YELLOW   lv_color_hex(0xFFD166)
#define C_RED      lv_color_hex(0xFF6B6B)
#define C_CYAN     lv_color_hex(0x55D6E8)

typedef struct {
    lv_obj_t *root;
    lv_obj_t *value;
    lv_obj_t *bar;
} metric_card_t;

static lv_obj_t *status_dot;
static lv_obj_t *status_text;
static lv_obj_t *wifi_dot;
static lv_obj_t *wifi_text;
static lv_obj_t *clock_label;
static lv_obj_t *hero_value;
static lv_obj_t *hero_card;
static lv_obj_t *hero_unit;
static lv_obj_t *hero_badge;
static lv_obj_t *hero_hint;
static lv_obj_t *splash;
static int64_t splash_started_us;
static bool splash_closing;
static lv_obj_t *chart;
static lv_chart_series_t *co2_series;
static lv_chart_series_t *pm_series;
static lv_obj_t *chart_title;
static lv_obj_t *toggle;
static lv_obj_t *chart_axis_labels[5];
static lv_obj_t *history_popup;
static lv_obj_t *history_chart;
static lv_chart_series_t *history_series;
static lv_obj_t *history_title;
static lv_obj_t *history_range_text;
static lv_obj_t *history_range_buttons[8];
static lv_obj_t *history_y_labels[5];
static lv_obj_t *history_x_labels[3];
static lv_obj_t *history_y_unit;
static metric_card_t cards[6];
static bool show_pm;
static unsigned history_tick;
static lv_obj_t *pages[3];
static lv_obj_t *settings_page;
static lv_obj_t *settings_menu;
static lv_obj_t *settings_popups[4];
static lv_obj_t *circle_co2;
static lv_obj_t *circle_core;
static lv_obj_t *circle_status;
static lv_obj_t *circle_values[6];
static lv_obj_t *circle_arcs[6];
static lv_obj_t *particle_values[4];
static lv_obj_t *ssid_input;
static lv_obj_t *password_input;
static lv_obj_t *time_input;
static lv_obj_t *on_input;
static lv_obj_t *off_input;
static lv_obj_t *timezone_dropdown;
static lv_obj_t *time_source_dropdown;
static lv_obj_t *brightness_slider;
static lv_obj_t *brightness_value;
static lv_obj_t *keyboard;
static lv_obj_t *active_input;
static int current_page;
static int brightness = 100;
static int clock_offset_seconds;
static int display_on_minute = 7 * 60;
static int display_off_minute = 23 * 60;
static bool backlight_on = true;
static bool time_configured;
static bool swipe_handled;
static bool view_transition_running;
static lv_point_t swipe_start;
static int last_schedule_minute = -1;
typedef struct { const char *normal; const char *symbol; lv_obj_t *label; } key_info_t;
static key_info_t key_info[48];
static int key_count;
static bool key_shift;
static bool key_symbols;
static bool manual_time;

#define HISTORY_METRICS 7
#define HISTORY_MINUTES (7 * 24 * 60)
#define HISTORY_GRAPH_POINTS 240
static float *long_history;
static unsigned long_history_head;
static unsigned long_history_count;
static int history_metric;
static int history_range_index;
static float co2_recent[60];
static unsigned co2_recent_head;
static unsigned co2_recent_count;
static const int history_ranges[] = {60, 180, 300, 720, 1440, 2880, 7200, 10080};
static const char *history_range_names[] = {"1h", "3h", "5h", "12h", "1d", "2d", "5d", "7d"};
static const char *history_metric_names[] = {"CO2", "PM2.5", "VOC", "NOx", "Temperature", "Humidity", "PM10"};
static const char *history_metric_units[] = {"ppm", "ug/m3", "index", "index", "deg C", "% RH", "ug/m3"};

static const char *timezone_names =
    "Europe/Berlin (auto DST)\nUTC-12:00\nUTC-11:00\nUTC-10:00\nUTC-09:30\nUTC-09:00\n"
    "UTC-08:00\nUTC-07:00\nUTC-06:00\nUTC-05:00\nUTC-04:00\nUTC-03:30\nUTC-03:00\n"
    "UTC-02:00\nUTC-01:00\nUTC+00:00\nUTC+01:00\nUTC+02:00\nUTC+03:00\nUTC+03:30\n"
    "UTC+04:00\nUTC+04:30\nUTC+05:00\nUTC+05:30\nUTC+05:45\nUTC+06:00\nUTC+06:30\n"
    "UTC+07:00\nUTC+08:00\nUTC+08:45\nUTC+09:00\nUTC+09:30\nUTC+10:00\nUTC+10:30\n"
    "UTC+11:00\nUTC+12:00\nUTC+12:45\nUTC+13:00\nUTC+14:00";
static const int timezone_offsets[] = {
    0, -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180,
    -120, -60, 0, 60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 390, 420,
    480, 525, 540, 570, 600, 630, 660, 720, 765, 780, 840
};
static int timezone_index;

static void apply_timezone(int index)
{
    if (index < 0 || index >= (int)(sizeof(timezone_offsets) / sizeof(timezone_offsets[0]))) index = 0;
    timezone_index = index;
    if (index == 0) {
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    } else {
        int offset = timezone_offsets[index];
        int west = -offset;
        char rule[20];
        snprintf(rule, sizeof(rule), "UTC%c%d:%02d", west < 0 ? '-' : '+', abs(west) / 60, abs(west) % 60);
        setenv("TZ", rule, 1);
    }
    tzset();
}

static int parse_time(const char *text, int fallback)
{
    int hour, minute;
    if (sscanf(text, "%d:%d", &hour, &minute) == 2 && hour >= 0 && hour < 24 && minute >= 0 && minute < 60)
        return hour * 60 + minute;
    return fallback;
}

static void format_time(char *out, size_t size, int minute)
{
    snprintf(out, size, "%02d:%02d", (minute / 60) % 24, minute % 60);
}

static int current_minute(void)
{
    time_t now = time(NULL);
    if (!manual_time && now > 1609459200) {
        struct tm local;
        localtime_r(&now, &local);
        return local.tm_hour * 60 + local.tm_min;
    }
    int seconds = (int)(esp_timer_get_time() / 1000000) + clock_offset_seconds;
    return ((seconds / 60) % 1440 + 1440) % 1440;
}

static void load_settings(void)
{
    nvs_handle_t handle;
    if (nvs_open("air-ui", NVS_READONLY, &handle) != ESP_OK) return;
    int32_t value;
    /* Always boot at full brightness. The slider still applies immediately
     * during the current session, but never makes the next boot unreadable. */
    if (nvs_get_i32(handle, "clock_offset", &value) == ESP_OK) {
        clock_offset_seconds = value;
        time_configured = true;
    }
    if (nvs_get_i32(handle, "display_on", &value) == ESP_OK) display_on_minute = value;
    if (nvs_get_i32(handle, "display_off", &value) == ESP_OK) display_off_minute = value;
    if (nvs_get_i32(handle, "timezone", &value) == ESP_OK) timezone_index = value;
    if (nvs_get_i32(handle, "time_source", &value) == ESP_OK) manual_time = value != 0;
    nvs_close(handle);
    apply_timezone(timezone_index);
}

typedef struct {
    lv_obj_t *outgoing;
    lv_obj_t *incoming;
} view_transition_t;

static void view_anim_x(void *object, int32_t value) { lv_obj_set_x(object, value); }
static void view_anim_y(void *object, int32_t value) { lv_obj_set_y(object, value); }
static void splash_opa(void *object, int32_t value) { lv_obj_set_style_opa(object, value, 0); }
static void splash_progress(void *object, int32_t value) { lv_bar_set_value(object, value, LV_ANIM_OFF); }

static void splash_finished(lv_anim_t *animation)
{
    (void)animation;
    if (splash) lv_obj_delete(splash);
    splash = NULL;
}

static void dismiss_splash(void)
{
    if (!splash || splash_closing) return;
    splash_closing = true;
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, splash);
    lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade, 420);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade, splash_opa);
    lv_anim_set_completed_cb(&fade, splash_finished);
    lv_anim_start(&fade);
}

static void view_transition_finished(lv_anim_t *animation)
{
    view_transition_t *transition = lv_anim_get_user_data(animation);
    if (transition->outgoing) {
        lv_obj_add_flag(transition->outgoing, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(transition->outgoing, 0, 0);
    }
    lv_obj_set_pos(transition->incoming, 0, 0);
    view_transition_running = false;
    free(transition);
}

static void animate_view(lv_obj_t *outgoing, lv_obj_t *incoming, bool vertical, int direction)
{
    if (view_transition_running || outgoing == incoming) return;
    view_transition_t *transition = calloc(1, sizeof(*transition));
    if (!transition) return;
    transition->outgoing = outgoing;
    transition->incoming = incoming;
    view_transition_running = true;

    int distance = vertical ? 480 : 800;
    int incoming_start = direction * distance;
    lv_obj_remove_flag(incoming, LV_OBJ_FLAG_HIDDEN);
    if (vertical) lv_obj_set_pos(incoming, 0, incoming_start);
    else lv_obj_set_pos(incoming, incoming_start, 0);
    lv_obj_move_foreground(incoming);

    lv_anim_t out;
    lv_anim_init(&out);
    lv_anim_set_var(&out, outgoing);
    lv_anim_set_values(&out, 0, -direction * distance);
    lv_anim_set_duration(&out, 310);
    lv_anim_set_path_cb(&out, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&out, vertical ? view_anim_y : view_anim_x);
    lv_anim_start(&out);

    lv_anim_t in;
    lv_anim_init(&in);
    lv_anim_set_var(&in, incoming);
    lv_anim_set_values(&in, incoming_start, 0);
    lv_anim_set_duration(&in, 310);
    lv_anim_set_path_cb(&in, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&in, vertical ? view_anim_y : view_anim_x);
    lv_anim_set_user_data(&in, transition);
    lv_anim_set_completed_cb(&in, view_transition_finished);
    lv_anim_start(&in);
}

static void show_page(int index)
{
    int next_page = (index + 3) % 3;
    bool settings_open = !lv_obj_has_flag(settings_page, LV_OBJ_FLAG_HIDDEN);
    if (settings_open) {
        animate_view(settings_page, pages[next_page], true, 1);
    } else if (next_page != current_page) {
        int direction = index > current_page ? 1 : -1;
        /* Handle the wrap-around as a continuous carousel. */
        if (current_page == 2 && next_page == 0) direction = 1;
        if (current_page == 0 && next_page == 2) direction = -1;
        animate_view(pages[current_page], pages[next_page], false, direction);
    }
    current_page = next_page;
}

/* Track the pointer ourselves as well. This remains reliable when a swipe starts
 * on a chart, card, arc or another child widget that consumes LVGL gestures. */
static void swipe_event(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &swipe_start);
        swipe_handled = false;
        return;
    }
    if (swipe_handled || view_transition_running) return;
    lv_point_t end;
    lv_indev_get_point(indev, &end);
    int dx = end.x - swipe_start.x;
    int dy = end.y - swipe_start.y;
    if (LV_ABS(dx) < 55 && LV_ABS(dy) < 55) return;
    swipe_handled = true;
    if (LV_ABS(dy) > LV_ABS(dx)) {
        if (dy > 0) {
            if (!lv_obj_has_flag(settings_page, LV_OBJ_FLAG_HIDDEN)) return;
            if (history_popup) lv_obj_add_flag(history_popup, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(settings_menu, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 4; ++i) lv_obj_add_flag(settings_popups[i], LV_OBJ_FLAG_HIDDEN);
            active_input = ssid_input;
            lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            animate_view(pages[current_page], settings_page, true, -1);
        } else if (!lv_obj_has_flag(settings_page, LV_OBJ_FLAG_HIDDEN)) {
            show_page(current_page);
        }
    } else if (lv_obj_has_flag(settings_page, LV_OBJ_FLAG_HIDDEN)) {
        show_page(current_page + (dx < 0 ? 1 : -1));
    }
}

static void textarea_focus(lv_event_t *event)
{
    active_input = lv_event_get_target_obj(event);
    lv_obj_move_foreground(keyboard);
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void key_event(lv_event_t *event)
{
    if (!active_input) return;
    key_info_t *info = lv_event_get_user_data(event);
    const char *key = info->normal;
    if (strcmp(key, "DEL") == 0) {
        lv_textarea_delete_char(active_input);
    } else if (strcmp(key, "SPACE") == 0) {
        lv_textarea_add_char(active_input, ' ');
    } else if (strcmp(key, "DONE") == 0) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    } else if (strcmp(key, "SHIFT") == 0) {
        key_shift = !key_shift;
        for (int i = 0; i < key_count; ++i) {
            const char *base = key_symbols && key_info[i].symbol ? key_info[i].symbol : key_info[i].normal;
            char shown[2] = {base[0], 0};
            if (!key_symbols && key_shift && base[1] == 0 && base[0] >= 'a' && base[0] <= 'z') shown[0] -= 32;
            lv_label_set_text(key_info[i].label, shown[0] && base[1] == 0 ? shown : base);
        }
    } else if (strcmp(key, "SYM") == 0) {
        key_symbols = !key_symbols;
        for (int i = 0; i < key_count; ++i) {
            const char *base = key_symbols && key_info[i].symbol ? key_info[i].symbol : key_info[i].normal;
            lv_label_set_text(key_info[i].label, base);
        }
    } else {
        const char *out = key_symbols && info->symbol ? info->symbol : key;
        char shifted[2] = {out[0], 0};
        if (!key_symbols && key_shift && out[1] == 0 && out[0] >= 'a' && out[0] <= 'z') shifted[0] -= 32;
        lv_textarea_add_text(active_input, shifted[0] && out[1] == 0 ? shifted : out);
    }
}

static void close_settings_event(lv_event_t *event)
{
    show_page(current_page);
    (void)event;
}

static void popup_event(lv_event_t *event)
{
    int selected = (int)(intptr_t)lv_event_get_user_data(event);
    lv_obj_add_flag(settings_menu, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; ++i) {
        if (i == selected) lv_obj_remove_flag(settings_popups[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(settings_popups[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void popup_back_event(lv_event_t *event)
{
    for (int i = 0; i < 4; ++i) lv_obj_add_flag(settings_popups[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(settings_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    (void)event;
}

static void brightness_event(lv_event_t *event)
{
    brightness = lv_slider_get_value(lv_event_get_target_obj(event));
    backlight_on = true;
    char text[12];
    snprintf(text, sizeof(text), "%d%%", brightness);
    lv_label_set_text(brightness_value, text);
    bsp_display_brightness_set(brightness);
}

static void time_source_event(lv_event_t *event)
{
    manual_time = lv_dropdown_get_selected(lv_event_get_target_obj(event)) == 1;
    if (manual_time) lv_obj_remove_flag(time_input, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(time_input, LV_OBJ_FLAG_HIDDEN);
}

static void save_event(lv_event_t *event)
{
    display_on_minute = parse_time(lv_textarea_get_text(on_input), display_on_minute);
    display_off_minute = parse_time(lv_textarea_get_text(off_input), display_off_minute);
    manual_time = lv_dropdown_get_selected(time_source_dropdown) == 1;
    if (manual_time) {
        int manual_minute = parse_time(lv_textarea_get_text(time_input), current_minute());
        int uptime_seconds = (int)(esp_timer_get_time() / 1000000);
        clock_offset_seconds = manual_minute * 60 - uptime_seconds;
    }
    time_configured = true;
    timezone_index = lv_dropdown_get_selected(timezone_dropdown);
    apply_timezone(timezone_index);
    nvs_handle_t handle;
    if (nvs_open("air-ui", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_i32(handle, "brightness", brightness);
        nvs_set_i32(handle, "clock_offset", clock_offset_seconds);
        nvs_set_i32(handle, "display_on", display_on_minute);
        nvs_set_i32(handle, "display_off", display_off_minute);
        nvs_set_i32(handle, "timezone", timezone_index);
        nvs_set_i32(handle, "time_source", manual_time ? 1 : 0);
        nvs_set_str(handle, "ssid", lv_textarea_get_text(ssid_input));
        nvs_set_str(handle, "password", lv_textarea_get_text(password_input));
        nvs_commit(handle);
        nvs_close(handle);
    }
    wifi_manager_connect(lv_textarea_get_text(ssid_input), lv_textarea_get_text(password_input));
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; ++i) lv_obj_add_flag(settings_popups[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(settings_menu, LV_OBJ_FLAG_HIDDEN);
    (void)event;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, color, 0);
    return obj;
}

static void panel(lv_obj_t *obj, lv_color_t color, int radius)
{
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *create_key(lv_obj_t *parent, const char *text, const char *symbol, int x, int y, int width)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 48);
    panel(button, C_PANEL, 9);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, C_MUTED, 0);
    lv_obj_set_style_bg_color(button, C_CYAN, LV_STATE_PRESSED);
    lv_obj_t *key_label = label(button, text, &lv_font_montserrat_14, C_TEXT);
    lv_obj_center(key_label);
    key_info[key_count] = (key_info_t){.normal = text, .symbol = symbol, .label = key_label};
    lv_obj_add_event_cb(button, key_event, LV_EVENT_CLICKED, &key_info[key_count++]);
    return button;
}

static lv_obj_t *create_keyboard(lv_obj_t *parent)
{
    lv_obj_t *board = lv_obj_create(parent);
    lv_obj_set_pos(board, 0, 250);
    lv_obj_set_size(board, 800, 230);
    panel(board, C_PANEL_2, 0);

    key_count = 0;
    static const char *row1[] = {"1","2","3","4","5","6","7","8","9","0"};
    static const char *sym1[] = {"!","\"","#","$","%","&","'","(",")","*"};
    static const char *row2[] = {"q","w","e","r","t","y","u","i","o","p"};
    static const char *sym2[] = {"+",",","/",":",";","<","=",">","?","~"};
    static const char *row3[] = {"a","s","d","f","g","h","j","k","l","DEL"};
    static const char *sym3[] = {"[","]","{","}","\\","|","^","_","`",NULL};
    for (int i = 0; i < 10; ++i) {
        create_key(board, row1[i], sym1[i], 5 + i * 79, 5, 74);
        create_key(board, row2[i], sym2[i], 5 + i * 79, 58, 74);
        create_key(board, row3[i], sym3[i], 5 + i * 79, 111, 74);
    }
    static const char *bottom[] = {"z","x","c","v","b","n","m"};
    static const char *sym4[] = {"@",".","-","_","+","=","?"};
    create_key(board, "SHIFT", NULL, 5, 164, 82);
    for (int i = 0; i < 7; ++i) create_key(board, bottom[i], sym4[i], 92 + i * 55, 164, 50);
    create_key(board, "SYM", NULL, 482, 164, 72);
    create_key(board, "SPACE", NULL, 559, 164, 117);
    create_key(board, "DONE", NULL, 681, 164, 109);
    return board;
}

static lv_color_t quality_color(const sen66_data_t *d)
{
    if ((!isnan(d->co2) && d->co2 > 1400) || (!isnan(d->pm25) && d->pm25 > 35) ||
        (!isnan(d->voc) && d->voc > 250) || (!isnan(d->nox) && d->nox > 250)) return C_RED;
    if ((!isnan(d->co2) && d->co2 > 1000) || (!isnan(d->pm25) && d->pm25 > 15) ||
        (!isnan(d->voc) && d->voc > 100) || (!isnan(d->nox) && d->nox > 100)) return C_YELLOW;
    return C_GREEN;
}

static const char *quality_text(const sen66_data_t *d)
{
    lv_color_t c = quality_color(d);
    if (lv_color_eq(c, C_RED)) return "VENTILATE";
    if (lv_color_eq(c, C_YELLOW)) return "FAIR";
    return "VERY GOOD";
}

static void format_value(char *out, size_t size, float value, int decimals)
{
    if (isnan(value)) snprintf(out, size, "--");
    else snprintf(out, size, decimals ? "%.1f" : "%.0f", value);
}

typedef struct {
    lv_obj_t *outgoing;
    lv_obj_t *incoming;
} slot_transition_t;

static void slot_translate(void *object, int32_t value)
{
    lv_obj_t *label_obj = object;
    lv_obj_set_style_translate_y(label_obj, value, 0);
    lv_obj_set_style_opa(label_obj, LV_CLAMP(55, 255 - LV_ABS(value) * 11, 255), 0);
}

static void slot_finished(lv_anim_t *animation)
{
    slot_transition_t *transition = lv_anim_get_user_data(animation);
    if (transition->outgoing) lv_obj_delete(transition->outgoing);
    if (transition->incoming) lv_obj_delete(transition->incoming);
    free(transition);
}

static lv_obj_t *slot_overlay(lv_obj_t *source, char digit, int32_t x, int32_t y)
{
    lv_obj_t *overlay = lv_label_create(lv_obj_get_parent(source));
    char text[2] = {digit, 0};
    const lv_font_t *font = lv_obj_get_style_text_font(source, LV_PART_MAIN);
    lv_label_set_text(overlay, text);
    lv_obj_set_pos(overlay, x, y);
    lv_obj_set_size(overlay, lv_font_get_glyph_width(font, digit, 0) + 2,
                    lv_obj_get_height(source));
    lv_obj_set_style_text_font(overlay, font, 0);
    lv_obj_set_style_text_color(overlay, lv_obj_get_style_text_color(source, LV_PART_MAIN), 0);
    lv_obj_set_style_text_align(overlay, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_move_foreground(overlay);
    return overlay;
}

static void animated_number_set(lv_obj_t *label_obj, const char *new_text)
{
    const char *old_text = lv_label_get_text(label_obj);
    if (strcmp(old_text, new_text) == 0) return;
    char *old_end = NULL;
    char *new_end = NULL;
    float old_value = strtof(old_text, &old_end);
    float new_value = strtof(new_text, &new_end);
    if (old_end == old_text || new_end == new_text) {
        lv_label_set_text(label_obj, new_text);
        return;
    }
    size_t old_len = strlen(old_text), new_len = strlen(new_text);
    /* A changed digit can only keep the same physical column if the formatted
     * value has the same number of characters. Avoid a misleading roll at
     * transitions such as 999 -> 1000. */
    if (old_len == 0 || old_len != new_len || old_len >= 23) {
        lv_label_set_text(label_obj, new_text);
        return;
    }
    char old_copy[24];
    strlcpy(old_copy, old_text, sizeof(old_copy));
    old_text = old_copy;
    int direction = new_value >= old_value ? 1 : -1;
    const lv_font_t *font = lv_obj_get_style_text_font(label_obj, LV_PART_MAIN);
    int32_t letter_space = lv_obj_get_style_text_letter_space(label_obj, LV_PART_MAIN);
    int32_t total_width = 0;
    for (size_t i = 0; i < old_len; ++i) {
        total_width += lv_font_get_glyph_width(font, old_text[i],
                                               i + 1 < old_len ? old_text[i + 1] : 0);
        if (i + 1 < old_len) total_width += letter_space;
    }
    int32_t origin_x = lv_obj_get_x(label_obj);
    lv_text_align_t align = lv_obj_get_style_text_align(label_obj, LV_PART_MAIN);
    if (align == LV_TEXT_ALIGN_CENTER) origin_x += (lv_obj_get_width(label_obj) - total_width) / 2;
    else if (align == LV_TEXT_ALIGN_RIGHT) origin_x += lv_obj_get_width(label_obj) - total_width;
    int32_t source_y = lv_obj_get_y(label_obj);

    /* Record exact glyph coordinates before changing the source label. */
    int32_t digit_x[24];
    int32_t cursor_x = origin_x;
    for (size_t i = 0; i < old_len; ++i) {
        digit_x[i] = cursor_x;
        cursor_x += lv_font_get_glyph_width(font, old_text[i],
                                            i + 1 < old_len ? old_text[i + 1] : 0);
        if (i + 1 < old_len) cursor_x += letter_space;
    }

    lv_label_set_text(label_obj, new_text);
    for (size_t i = 0; i < old_len; ++i) {
        if (old_text[i] == new_text[i] || old_text[i] < '0' || old_text[i] > '9' ||
            new_text[i] < '0' || new_text[i] > '9') continue;
        slot_transition_t *transition = calloc(1, sizeof(*transition));
        if (!transition) continue;
        transition->outgoing = slot_overlay(label_obj, old_text[i], digit_x[i], source_y);
        transition->incoming = slot_overlay(label_obj, new_text[i], digit_x[i], source_y);
        slot_translate(transition->incoming, direction * 20);

        lv_anim_t outgoing;
        lv_anim_init(&outgoing);
        lv_anim_set_var(&outgoing, transition->outgoing);
        lv_anim_set_values(&outgoing, 0, -direction * 20);
        lv_anim_set_duration(&outgoing, 220);
        lv_anim_set_path_cb(&outgoing, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&outgoing, slot_translate);
        lv_anim_start(&outgoing);

        lv_anim_t incoming;
        lv_anim_init(&incoming);
        lv_anim_set_var(&incoming, transition->incoming);
        lv_anim_set_values(&incoming, direction * 20, 0);
        lv_anim_set_duration(&incoming, 220);
        lv_anim_set_path_cb(&incoming, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&incoming, slot_translate);
        lv_anim_set_user_data(&incoming, transition);
        lv_anim_set_completed_cb(&incoming, slot_finished);
        lv_anim_start(&incoming);
    }
}

static void format_history_duration(char *out, size_t size, int minutes)
{
    if (minutes >= 1440 && minutes % 1440 == 0) snprintf(out, size, "-%dd", minutes / 1440);
    else if (minutes >= 60 && minutes % 60 == 0) snprintf(out, size, "-%dh", minutes / 60);
    else snprintf(out, size, "-%dmin", minutes);
}

static void refresh_history_graph(void)
{
    if (!history_chart || !long_history) return;
    unsigned wanted = history_ranges[history_range_index];
    unsigned available = LV_MIN(long_history_count, wanted);
    float minimum = INFINITY, maximum = -INFINITY;
    lv_chart_set_point_count(history_chart, HISTORY_GRAPH_POINTS);
    lv_chart_set_all_value(history_chart, history_series, LV_CHART_POINT_NONE);
    for (int p = 0; p < HISTORY_GRAPH_POINTS; ++p) {
        unsigned from = (unsigned)((uint64_t)p * available / HISTORY_GRAPH_POINTS);
        unsigned to = (unsigned)((uint64_t)(p + 1) * available / HISTORY_GRAPH_POINTS);
        if (to <= from) to = from + 1;
        float sum = 0; unsigned valid = 0;
        for (unsigned j = from; j < to && j < available; ++j) {
            unsigned oldest = (long_history_head + HISTORY_MINUTES - available) % HISTORY_MINUTES;
            unsigned slot = (oldest + j) % HISTORY_MINUTES;
            float value = long_history[slot * HISTORY_METRICS + history_metric];
            if (!isnan(value)) { sum += value; ++valid; }
        }
        int32_t plotted = LV_CHART_POINT_NONE;
        if (valid) {
            float value = sum / valid;
            minimum = LV_MIN(minimum, value); maximum = LV_MAX(maximum, value);
            plotted = (int32_t)lroundf(value);
        }
        lv_chart_set_next_value(history_chart, history_series, plotted);
    }
    if (!isfinite(minimum)) { minimum = 0; maximum = 100; }
    if (history_metric == 0) minimum = LV_MIN(400.0f, minimum);
    float span = maximum - minimum;
    if (span < 10) span = 10;
    int axis_min = (int)floorf(minimum - span * 0.12f);
    int axis_max = (int)ceilf(maximum + span * 0.12f);
    if (history_metric == 0) axis_min = 400;
    lv_chart_set_range(history_chart, LV_CHART_AXIS_PRIMARY_Y, axis_min, axis_max);
    char text[80];
    snprintf(text, sizeof(text), "%s HISTORY", history_metric_names[history_metric]);
    lv_label_set_text(history_title, text);
    snprintf(text, sizeof(text), "%s  |  %d to %d  |  %u min recorded",
             history_range_names[history_range_index], axis_min, axis_max, available);
    lv_label_set_text(history_range_text, text);
    for (int i = 0; i < 5; ++i) {
        int value = axis_max - (axis_max - axis_min) * i / 4;
        snprintf(text, sizeof(text), "%d", value);
        lv_label_set_text(history_y_labels[i], text);
    }
    lv_label_set_text(history_y_unit, history_metric_units[history_metric]);
    unsigned actual_minutes = available > 1 ? available - 1 : 0;
    if (actual_minutes == 0) {
        lv_label_set_text(history_x_labels[0], "start");
        lv_label_set_text(history_x_labels[1], "");
    } else {
        format_history_duration(text, sizeof(text), actual_minutes);
        lv_label_set_text(history_x_labels[0], text);
        if (actual_minutes >= 2) {
            format_history_duration(text, sizeof(text), actual_minutes / 2);
            lv_label_set_text(history_x_labels[1], text);
        } else lv_label_set_text(history_x_labels[1], "");
    }
    lv_label_set_text(history_x_labels[2], "now");
    lv_chart_refresh(history_chart);
}

static void history_metric_event(lv_event_t *event)
{
    /* LVGL can still emit CLICKED for the card on which a swipe began.
     * The global pointer tracker has already classified that interaction. */
    if (swipe_handled || view_transition_running) return;
    history_metric = (int)(intptr_t)lv_event_get_user_data(event);
    lv_obj_remove_flag(history_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(history_popup);
    refresh_history_graph();
}

static void history_range_event(lv_event_t *event)
{
    history_range_index = (int)(intptr_t)lv_event_get_user_data(event);
    for (int i = 0; i < 8; ++i) {
        lv_obj_set_style_bg_color(history_range_buttons[i], i == history_range_index ? C_CYAN : C_PANEL_2, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(history_range_buttons[i], 0),
                                    i == history_range_index ? C_BG : C_TEXT, 0);
    }
    refresh_history_graph();
}

static void history_close_event(lv_event_t *event)
{
    lv_obj_add_flag(history_popup, LV_OBJ_FLAG_HIDDEN);
    (void)event;
}

static void update_main_co2_scale(void)
{
    if (show_pm) return;
    float minimum = INFINITY;
    float maximum = -INFINITY;
    for (unsigned i = 0; i < co2_recent_count; ++i) {
        float value = co2_recent[i];
        if (!isnan(value)) {
            minimum = LV_MIN(minimum, value);
            maximum = LV_MAX(maximum, value);
        }
    }
    int axis_min = 400;
    int axis_max = 800;
    if (isfinite(minimum) && isfinite(maximum)) {
        float span = maximum - minimum;
        float padding = LV_MAX(10.0f, span * 0.20f);
        if (span < 40.0f) padding = (50.0f - span) * 0.5f;
        axis_min = LV_MAX(400, ((int)floorf((minimum - padding) / 10.0f)) * 10);
        axis_max = ((int)ceilf((maximum + padding) / 10.0f)) * 10;
        if (axis_max - axis_min < 50) axis_max = axis_min + 50;
    }
    char text[12];
    for (int i = 0; i < 5; ++i) {
        int value = axis_max - (axis_max - axis_min) * i / 4;
        snprintf(text, sizeof(text), "%d", value);
        lv_label_set_text(chart_axis_labels[i], text);
    }
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, axis_min, axis_max);
}

static void toggle_event(lv_event_t *event)
{
    (void)event;
    show_pm = !show_pm;
    lv_label_set_text(chart_title, show_pm ? "PM2.5 - LAST 60 MIN" : "CO2 - LAST 60 MIN");
    lv_label_set_text(lv_obj_get_child(toggle, 0), show_pm ? "PM2.5" : "CO2");
    lv_chart_hide_series(chart, co2_series, show_pm);
    lv_chart_hide_series(chart, pm_series, !show_pm);
    if (show_pm) lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 60);
    static const char *pm_scale[] = {"60", "45", "30", "15", "0"};
    if (show_pm) {
        for (int i = 0; i < 5; ++i) lv_label_set_text(chart_axis_labels[i], pm_scale[i]);
    } else update_main_co2_scale();
    lv_chart_refresh(chart);
}

static void make_metric(lv_obj_t *parent, int index, int x, int y,
                        const char *name, const char *unit, lv_color_t accent)
{
    lv_obj_t *card = lv_obj_create(parent);
    cards[index].root = card;
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, 157, 92);
    panel(card, C_PANEL, 18);
    lv_obj_t *name_label = label(card, name, &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(name_label, 14, 12);
    cards[index].value = label(card, "--", &lv_font_montserrat_28, C_TEXT);
    lv_obj_set_pos(cards[index].value, 14, 32);
    lv_obj_t *unit_label = label(card, unit, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(unit_label, LV_ALIGN_BOTTOM_RIGHT, -13, -13);
    cards[index].bar = lv_obj_create(card);
    lv_obj_set_pos(cards[index].bar, 0, 0);
    lv_obj_set_size(cards[index].bar, 5, 92);
    panel(cards[index].bar, accent, 3);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, history_metric_event, LV_EVENT_CLICKED, (void *)(intptr_t)(index + 1));
}

void air_ui_create(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, C_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_indev_t *touch = lv_indev_get_next(NULL);
    if (touch) {
        lv_indev_add_event_cb(touch, swipe_event, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(touch, swipe_event, LV_EVENT_PRESSING, NULL);
        lv_indev_add_event_cb(touch, swipe_event, LV_EVENT_RELEASED, NULL);
    }

    load_settings();
    for (int i = 0; i < 3; ++i) {
        pages[i] = lv_obj_create(screen);
        lv_obj_set_pos(pages[i], 0, 0);
        lv_obj_set_size(pages[i], 800, 480);
        panel(pages[i], C_BG, 0);
        if (i) lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *page_main = pages[0];

    lv_obj_t *brand = label(page_main, "Airstation", &lv_font_montserrat_22, C_TEXT);
    lv_obj_set_pos(brand, 24, 18);
    clock_label = label(page_main, "--:--", &lv_font_montserrat_22, C_TEXT);
    lv_obj_align(clock_label, LV_ALIGN_TOP_MID, 0, 18);

    status_dot = lv_obj_create(page_main);
    lv_obj_set_pos(status_dot, 570, 27);
    lv_obj_set_size(status_dot, 9, 9);
    panel(status_dot, C_YELLOW, LV_RADIUS_CIRCLE);
    status_text = label(page_main, "Sensor", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(status_text, 586, 22);
    wifi_dot = lv_obj_create(page_main);
    lv_obj_set_pos(wifi_dot, 674, 27);
    lv_obj_set_size(wifi_dot, 9, 9);
    panel(wifi_dot, C_RED, LV_RADIUS_CIRCLE);
    wifi_text = label(page_main, "WiFi", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(wifi_text, 690, 22);
    hero_card = lv_obj_create(page_main);
    lv_obj_set_pos(hero_card, 20, 68);
    lv_obj_set_size(hero_card, 244, 214);
    panel(hero_card, C_PANEL_2, 24);
    lv_obj_add_flag(hero_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hero_card, history_metric_event, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    label(hero_card, "AIR QUALITY", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(lv_obj_get_child(hero_card, 0), 18, 17);
    hero_value = label(hero_card, "--", &lv_font_montserrat_48, C_TEXT);
    lv_obj_set_pos(hero_value, 18, 55);
    hero_unit = label(hero_card, "ppm CO2", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(hero_unit, 20, 112);
    hero_badge = lv_obj_create(hero_card);
    lv_obj_set_pos(hero_badge, 18, 145);
    lv_obj_set_size(hero_badge, 114, 36);
    panel(hero_badge, C_YELLOW, 18);
    lv_obj_t *badge_text = label(hero_badge, "WAITING", &lv_font_montserrat_12, C_BG);
    lv_obj_center(badge_text);
    hero_hint = label(hero_card, "Preparing first measurement", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(hero_hint, 18, 188);

    make_metric(page_main, 0, 280, 68,  "PM2.5", "ug/m3", C_CYAN);
    make_metric(page_main, 1, 449, 68,  "VOC INDEX", "Index", C_GREEN);
    make_metric(page_main, 2, 618, 68,  "NOx INDEX", "Index", C_YELLOW);
    make_metric(page_main, 3, 280, 174, "TEMPERATURE", "deg C", C_RED);
    make_metric(page_main, 4, 449, 174, "HUMIDITY", "% RH", C_CYAN);
    make_metric(page_main, 5, 618, 174, "PM10", "ug/m3", C_YELLOW);

    lv_obj_t *chart_panel = lv_obj_create(page_main);
    lv_obj_set_pos(chart_panel, 20, 298);
    lv_obj_set_size(chart_panel, 760, 166);
    panel(chart_panel, C_PANEL, 22);
    chart_title = label(chart_panel, "CO2 - LAST 60 MIN", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(chart_title, 18, 15);

    toggle = lv_button_create(chart_panel);
    lv_obj_set_pos(toggle, 668, 10);
    lv_obj_set_size(toggle, 76, 31);
    panel(toggle, C_PANEL_2, 15);
    lv_obj_add_event_cb(toggle, toggle_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *toggle_text = label(toggle, "CO2", &lv_font_montserrat_12, C_TEXT);
    lv_obj_center(toggle_text);

    chart = lv_chart_create(chart_panel);
    lv_obj_set_pos(chart, 62, 49);
    lv_obj_set_size(chart, 686, 88);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x253944), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 60);
    lv_chart_set_div_line_count(chart, 3, 6);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 400, 2000);
    static const char *initial_scale[] = {"2000", "1600", "1200", "800", "400"};
    static const int axis_y[] = {43, 64, 85, 106, 127};
    for (int i = 0; i < 5; ++i) {
        chart_axis_labels[i] = label(chart_panel, initial_scale[i], &lv_font_montserrat_12, C_MUTED);
        lv_obj_set_width(chart_axis_labels[i], 46);
        lv_obj_set_style_text_align(chart_axis_labels[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(chart_axis_labels[i], 8, axis_y[i]);
    }
    lv_obj_t *history_start = label(chart_panel, "-60 min", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(history_start, 62, 145);
    lv_obj_t *history_now = label(chart_panel, "now", &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(history_now, LV_ALIGN_BOTTOM_RIGHT, -12, -5);
    co2_series = lv_chart_add_series(chart, C_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    pm_series = lv_chart_add_series(chart, C_CYAN, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_hide_series(chart, pm_series, true);
    for (int i = 0; i < 60; ++i) {
        co2_recent[i] = NAN;
        lv_chart_set_next_value(chart, co2_series, LV_CHART_POINT_NONE);
        lv_chart_set_next_value(chart, pm_series, LV_CHART_POINT_NONE);
    }

    /* Circular instrument view: a luminous six-segment orbital dashboard. */
    lv_obj_t *circle_page = pages[1];
    lv_obj_t *circle_title = label(circle_page, "AIR ORBIT", &lv_font_montserrat_22, C_TEXT);
    lv_obj_set_pos(circle_title, 24, 20);
    lv_obj_t *circle_hint = label(circle_page, "LIVE ENVIRONMENTAL MATRIX", &lv_font_montserrat_12, C_CYAN);
    lv_obj_set_pos(circle_hint, 24, 49);
    lv_obj_t *page_mark = label(circle_page, "02 / 03", &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(page_mark, LV_ALIGN_TOP_RIGHT, -26, 28);

    const char *arc_names[] = {"PM2.5", "VOC", "NOx", "TEMP", "HUM", "PM10"};
    const lv_color_t arc_colors[] = {C_CYAN, C_GREEN, C_YELLOW, C_RED, C_CYAN, C_YELLOW};
    static const lv_point_t satellite_pos[] = {
        {55,190}, {563,326}, {125,326}, {125,58}, {563,58}, {633,190}
    };

    lv_obj_t *orbit_outer = lv_arc_create(circle_page);
    lv_obj_set_pos(orbit_outer, 245, 65); lv_obj_set_size(orbit_outer, 310, 310);
    lv_arc_set_bg_angles(orbit_outer, 0, 360); lv_arc_set_value(orbit_outer, 0);
    lv_obj_remove_style(orbit_outer, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(orbit_outer, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(orbit_outer, lv_color_hex(0x28424B), LV_PART_MAIN);
    lv_obj_clear_flag(orbit_outer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *orbit_mid = lv_arc_create(circle_page);
    lv_obj_set_pos(orbit_mid, 275, 95); lv_obj_set_size(orbit_mid, 250, 250);
    lv_arc_set_bg_angles(orbit_mid, 0, 360); lv_arc_set_value(orbit_mid, 0);
    lv_obj_remove_style(orbit_mid, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(orbit_mid, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_color(orbit_mid, lv_color_hex(0x31525B), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(orbit_mid, LV_OPA_60, LV_PART_MAIN);
    lv_obj_clear_flag(orbit_mid, LV_OBJ_FLAG_CLICKABLE);

    const lv_color_t spectrum[] = {C_GREEN, lv_color_hex(0xB7F34A), C_YELLOW, C_RED, lv_color_hex(0xB55CFF)};
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *band = lv_arc_create(circle_page);
        lv_obj_set_pos(band, 260, 80); lv_obj_set_size(band, 280, 280);
        lv_arc_set_bg_angles(band, 205 + i * 26, 229 + i * 26);
        lv_arc_set_range(band, 0, 100); lv_arc_set_value(band, 100);
        lv_obj_remove_style(band, NULL, LV_PART_KNOB);
        lv_obj_set_style_arc_width(band, 17, LV_PART_MAIN);
        lv_obj_set_style_arc_width(band, 17, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(band, spectrum[i], LV_PART_MAIN);
        lv_obj_set_style_arc_color(band, spectrum[i], LV_PART_INDICATOR);
        lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
    }

    for (int i = 0; i < 6; ++i) {
        lv_obj_t *satellite = lv_obj_create(circle_page);
        lv_obj_set_pos(satellite, satellite_pos[i].x, satellite_pos[i].y);
        lv_obj_set_size(satellite, 112, 112);
        panel(satellite, lv_color_hex(0x0B171E), LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_width(satellite, 1, 0);
        lv_obj_set_style_border_color(satellite, arc_colors[i], 0);
        lv_obj_set_style_border_opa(satellite, LV_OPA_50, 0);
        lv_obj_set_style_shadow_width(satellite, 18, 0);
        lv_obj_set_style_shadow_color(satellite, arc_colors[i], 0);
        lv_obj_set_style_shadow_opa(satellite, LV_OPA_20, 0);
        circle_arcs[i] = lv_arc_create(circle_page);
        lv_obj_set_pos(circle_arcs[i], satellite_pos[i].x, satellite_pos[i].y);
        lv_obj_set_size(circle_arcs[i], 112, 112);
        lv_arc_set_bg_angles(circle_arcs[i], 0, 360);
        lv_arc_set_range(circle_arcs[i], 0, 100);
        lv_arc_set_value(circle_arcs[i], 0);
        lv_obj_remove_style(circle_arcs[i], NULL, LV_PART_KNOB);
        lv_obj_set_style_arc_width(circle_arcs[i], 2, LV_PART_MAIN);
        lv_obj_set_style_arc_color(circle_arcs[i], lv_color_hex(0x31464E), LV_PART_MAIN);
        lv_obj_set_style_arc_width(circle_arcs[i], 5, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(circle_arcs[i], arc_colors[i], LV_PART_INDICATOR);
        lv_obj_clear_flag(circle_arcs[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *satellite_name = label(circle_page, arc_names[i], &lv_font_montserrat_12, arc_colors[i]);
        lv_obj_set_width(satellite_name, 104);
        lv_obj_set_style_text_align(satellite_name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(satellite_name, satellite_pos[i].x + 4, satellite_pos[i].y + 19);
        circle_values[i] = label(circle_page, "--", &lv_font_montserrat_22, C_TEXT);
        lv_obj_set_width(circle_values[i], 104);
        lv_obj_set_style_text_align(circle_values[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(circle_values[i], satellite_pos[i].x + 4, satellite_pos[i].y + 42);
        static const char *satellite_units[] = {"ug/m3", "index", "index", "deg C", "% RH", "ug/m3"};
        lv_obj_t *satellite_unit = label(circle_page, satellite_units[i], &lv_font_montserrat_12, C_MUTED);
        lv_obj_set_width(satellite_unit, 104);
        lv_obj_set_style_text_align(satellite_unit, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(satellite_unit, satellite_pos[i].x + 4, satellite_pos[i].y + 75);
    }

    static const lv_point_t node_pos[] = {{242,240},{520,337},{278,337},{286,112},{508,112},{550,240}};
    for (int i = 0; i < 6; ++i) {
        lv_obj_t *node = lv_obj_create(circle_page);
        lv_obj_set_pos(node, node_pos[i].x, node_pos[i].y); lv_obj_set_size(node, 10, 10);
        panel(node, arc_colors[i], LV_RADIUS_CIRCLE);
        lv_obj_set_style_shadow_width(node, 14, 0);
        lv_obj_set_style_shadow_color(node, arc_colors[i], 0);
        lv_obj_set_style_shadow_opa(node, LV_OPA_70, 0);
    }

    circle_core = lv_obj_create(circle_page);
    lv_obj_set_pos(circle_core, 295, 115); lv_obj_set_size(circle_core, 210, 210);
    panel(circle_core, C_PANEL_2, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(circle_core, 2, 0);
    lv_obj_set_style_border_color(circle_core, lv_color_hex(0x2B6673), 0);
    lv_obj_set_style_shadow_width(circle_core, 28, 0);
    lv_obj_set_style_shadow_color(circle_core, C_CYAN, 0);
    lv_obj_set_style_shadow_opa(circle_core, LV_OPA_20, 0);
    lv_obj_t *core_caption = label(circle_core, "AIR QUALITY", &lv_font_montserrat_12, C_CYAN);
    lv_obj_align(core_caption, LV_ALIGN_TOP_MID, 0, 36);
    circle_co2 = label(circle_page, "--", &lv_font_montserrat_48, C_TEXT);
    lv_obj_align(circle_co2, LV_ALIGN_CENTER, 0, -14);
    lv_obj_t *circle_unit = label(circle_page, "ppm CO2", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(circle_unit, LV_ALIGN_CENTER, 0, 34);
    circle_status = label(circle_page, "WAITING", &lv_font_montserrat_14, C_YELLOW);
    lv_obj_set_style_bg_color(circle_status, C_PANEL, 0);
    lv_obj_set_style_bg_opa(circle_status, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(circle_status, 14, 0);
    lv_obj_set_style_pad_ver(circle_status, 6, 0);
    lv_obj_set_style_radius(circle_status, 14, 0);
    lv_obj_align(circle_status, LV_ALIGN_CENTER, 0, 73);

    /* A calm detail page for the four particle fractions. */
    lv_obj_t *detail = pages[2];
    lv_obj_t *detail_title = label(detail, "PARTICLE DETAIL", &lv_font_montserrat_22, C_TEXT);
    lv_obj_set_pos(detail_title, 24, 22);
    const char *particle_names[] = {"PM1.0", "PM2.5", "PM4.0", "PM10"};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *card = lv_obj_create(detail);
        lv_obj_set_pos(card, 24 + i * 190, 92);
        lv_obj_set_size(card, 174, 270);
        panel(card, i % 2 ? C_PANEL_2 : C_PANEL, 24);
        lv_obj_t *name = label(card, particle_names[i], &lv_font_montserrat_18, C_MUTED);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 24);
        particle_values[i] = label(card, "--", &lv_font_montserrat_36, C_TEXT);
        lv_obj_align(particle_values[i], LV_ALIGN_CENTER, 0, -5);
        lv_obj_t *particle_unit = label(card, "ug/m3", &lv_font_montserrat_12, C_MUTED);
        lv_obj_align(particle_unit, LV_ALIGN_CENTER, 0, 38);
        lv_obj_t *mark = label(card, "LIVE", &lv_font_montserrat_12, arc_colors[i]);
        lv_obj_align(mark, LV_ALIGN_BOTTOM_MID, 0, -26);
    }

    /* Pull-down settings dashboard with focused popups. */
    settings_page = lv_obj_create(screen);
    lv_obj_set_size(settings_page, 800, 480);
    panel(settings_page, C_BG, 0);
    lv_obj_add_flag(settings_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *settings_title = label(settings_page, "SETTINGS", &lv_font_montserrat_24, C_TEXT);
    lv_obj_set_pos(settings_title, 24, 18);
    lv_obj_t *settings_hint = label(settings_page, "Swipe up to close", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(settings_hint, 24, 49);
    lv_obj_t *close = lv_button_create(settings_page);
    lv_obj_set_pos(close, 716, 16);
    lv_obj_set_size(close, 60, 42);
    panel(close, C_PANEL_2, 14);
    lv_obj_t *close_text = label(close, "X", &lv_font_montserrat_18, C_TEXT);
    lv_obj_center(close_text);
    lv_obj_add_event_cb(close, close_settings_event, LV_EVENT_CLICKED, NULL);

    settings_menu = lv_obj_create(settings_page);
    lv_obj_set_pos(settings_menu, 0, 70); lv_obj_set_size(settings_menu, 800, 410); panel(settings_menu, C_BG, 0);
    const char *menu_names[] = {"WiFi", "TIME", "DISPLAY SCHEDULE", "BRIGHTNESS"};
    const char *menu_hints[] = {"Network and password", "NTP, manual time, zone", "Automatic on and off", "Display intensity"};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *button = lv_button_create(settings_menu);
        lv_obj_set_pos(button, 24 + (i % 2) * 382, 18 + (i / 2) * 158);
        lv_obj_set_size(button, 358, 138); panel(button, i % 2 ? C_PANEL_2 : C_PANEL, 22);
        lv_obj_set_style_border_width(button, 1, 0); lv_obj_set_style_border_color(button, C_PANEL_2, 0);
        lv_obj_t *title = label(button, menu_names[i], &lv_font_montserrat_18, C_TEXT); lv_obj_set_pos(title, 22, 28);
        lv_obj_t *hint = label(button, menu_hints[i], &lv_font_montserrat_12, C_MUTED); lv_obj_set_pos(hint, 22, 66);
        lv_obj_t *arrow = label(button, ">", &lv_font_montserrat_22, C_CYAN); lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -24, 0);
        lv_obj_add_event_cb(button, popup_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    for (int i = 0; i < 4; ++i) {
        settings_popups[i] = lv_obj_create(settings_page);
        lv_obj_set_pos(settings_popups[i], 0, 0); lv_obj_set_size(settings_popups[i], 800, 480);
        panel(settings_popups[i], C_BG, 0); lv_obj_add_flag(settings_popups[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *back = lv_button_create(settings_popups[i]);
        lv_obj_set_pos(back, 24, 18); lv_obj_set_size(back, 58, 42); panel(back, C_PANEL_2, 14);
        lv_obj_t *back_text = label(back, "<", &lv_font_montserrat_18, C_TEXT); lv_obj_center(back_text);
        lv_obj_add_event_cb(back, popup_back_event, LV_EVENT_CLICKED, NULL);
        lv_obj_t *popup_title = label(settings_popups[i], menu_names[i], &lv_font_montserrat_24, C_TEXT);
        lv_obj_set_pos(popup_title, 104, 23);
        lv_obj_t *save = lv_button_create(settings_popups[i]);
        lv_obj_set_pos(save, 528, 394); lv_obj_set_size(save, 248, 58); panel(save, C_GREEN, 18);
        lv_obj_t *save_text = label(save, "SAVE", &lv_font_montserrat_14, C_BG); lv_obj_center(save_text);
        lv_obj_add_event_cb(save, save_event, LV_EVENT_CLICKED, NULL);
    }

    ssid_input = lv_textarea_create(settings_popups[0]);
    password_input = lv_textarea_create(settings_popups[0]);
    time_input = lv_textarea_create(settings_popups[1]);
    on_input = lv_textarea_create(settings_popups[2]);
    off_input = lv_textarea_create(settings_popups[2]);
    timezone_dropdown = lv_dropdown_create(settings_popups[1]);
    time_source_dropdown = lv_dropdown_create(settings_popups[1]);
    lv_obj_t *fields[] = {ssid_input, password_input, time_input, on_input, off_input};
    const char *placeholders[] = {"WiFi SSID", "WiFi password", "Time HH:MM", "Turn on at HH:MM", "Turn off at HH:MM"};
    const lv_point_t field_pos[] = {{120,125},{120,195},{120,175},{120,165},{420,165}};
    char time_text[8];
    format_time(time_text, sizeof(time_text), current_minute());
    lv_textarea_set_text(time_input, time_text);
    format_time(time_text, sizeof(time_text), display_on_minute); lv_textarea_set_text(on_input, time_text);
    format_time(time_text, sizeof(time_text), display_off_minute); lv_textarea_set_text(off_input, time_text);
    nvs_handle_t settings_handle;
    if (nvs_open("air-ui", NVS_READONLY, &settings_handle) == ESP_OK) {
        char saved[65]; size_t length = sizeof(saved);
        if (nvs_get_str(settings_handle, "ssid", saved, &length) == ESP_OK) lv_textarea_set_text(ssid_input, saved);
        length = sizeof(saved);
        if (nvs_get_str(settings_handle, "password", saved, &length) == ESP_OK) lv_textarea_set_text(password_input, saved);
        nvs_close(settings_handle);
    }
    for (int i = 0; i < 5; ++i) {
        lv_obj_set_pos(fields[i], field_pos[i].x, field_pos[i].y);
        lv_obj_set_size(fields[i], i <= 2 ? 560 : 350, 46);
        lv_textarea_set_one_line(fields[i], true);
        lv_textarea_set_placeholder_text(fields[i], placeholders[i]);
        lv_obj_set_style_bg_color(fields[i], C_PANEL, 0);
        lv_obj_set_style_text_color(fields[i], C_TEXT, 0);
        lv_obj_set_style_border_width(fields[i], 0, 0);
        lv_obj_set_style_bg_color(fields[i], C_CYAN, LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(fields[i], LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_set_style_anim_duration(fields[i], 500, LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_add_event_cb(fields[i], textarea_focus, LV_EVENT_PRESSED, NULL);
    }
    lv_textarea_set_password_mode(password_input, true);
    lv_obj_t *schedule_help = label(settings_popups[2],
        "Daily schedule: the display turns on at the first time and off at the second.",
        &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(schedule_help, 120, 82);
    lv_obj_t *schedule_note = label(settings_popups[2],
        "Only the screen is switched off. Sensor measurements continue in the background.",
        &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(schedule_note, 120, 104);
    lv_obj_t *on_label = label(settings_popups[2], "DISPLAY ON", &lv_font_montserrat_12, C_GREEN);
    lv_obj_set_pos(on_label, 120, 140);
    lv_obj_t *off_label = label(settings_popups[2], "DISPLAY OFF", &lv_font_montserrat_12, C_RED);
    lv_obj_set_pos(off_label, 420, 140);
    lv_dropdown_set_options(time_source_dropdown, "Internet time (NTP)\nManual time");
    lv_dropdown_set_selected(time_source_dropdown, manual_time ? 1 : 0);
    lv_obj_set_pos(time_source_dropdown, 120, 105);
    lv_obj_set_size(time_source_dropdown, 560, 48);
    lv_obj_set_style_bg_color(time_source_dropdown, C_PANEL, 0);
    lv_obj_set_style_text_color(time_source_dropdown, C_TEXT, 0);
    lv_obj_set_style_border_width(time_source_dropdown, 0, 0);
    lv_obj_add_event_cb(time_source_dropdown, time_source_event, LV_EVENT_VALUE_CHANGED, NULL);
    if (!manual_time) lv_obj_add_flag(time_input, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *timezone_label = label(settings_popups[1], "TIME ZONE", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(timezone_label, 120, 238);
    lv_dropdown_set_options(timezone_dropdown, timezone_names);
    lv_dropdown_set_selected(timezone_dropdown, timezone_index);
    lv_obj_set_pos(timezone_dropdown, 120, 260);
    lv_obj_set_size(timezone_dropdown, 560, 48);
    lv_obj_set_style_bg_color(timezone_dropdown, C_PANEL, 0);
    lv_obj_set_style_text_color(timezone_dropdown, C_TEXT, 0);
    lv_obj_set_style_border_width(timezone_dropdown, 0, 0);
    lv_obj_t *bright_label = label(settings_popups[3], "DISPLAY BRIGHTNESS", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(bright_label, 120, 135);
    brightness_slider = lv_slider_create(settings_popups[3]);
    lv_obj_set_pos(brightness_slider, 120, 205);
    lv_obj_set_size(brightness_slider, 560, 24);
    lv_slider_set_range(brightness_slider, 25, 100);
    lv_slider_set_value(brightness_slider, brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(brightness_slider, brightness_event, LV_EVENT_VALUE_CHANGED, NULL);
    brightness_value = label(settings_popups[3], "", &lv_font_montserrat_22, C_TEXT);
    lv_obj_set_pos(brightness_value, 620, 128);
    char percent[12]; snprintf(percent, sizeof(percent), "%d%%", brightness); lv_label_set_text(brightness_value, percent);
    keyboard = create_keyboard(settings_page);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    active_input = ssid_input;
    for (int i = 0; i < 5; ++i) {
        lv_obj_add_event_cb(fields[i], textarea_focus, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(fields[i], textarea_focus, LV_EVENT_CLICKED, NULL);
    }

    long_history = heap_caps_calloc(HISTORY_MINUTES * HISTORY_METRICS, sizeof(float),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (long_history) {
        for (int i = 0; i < HISTORY_MINUTES * HISTORY_METRICS; ++i) long_history[i] = NAN;
    }
    history_popup = lv_obj_create(screen);
    lv_obj_set_pos(history_popup, 0, 0); lv_obj_set_size(history_popup, 800, 480);
    panel(history_popup, C_BG, 0); lv_obj_add_flag(history_popup, LV_OBJ_FLAG_HIDDEN);
    history_title = label(history_popup, "HISTORY", &lv_font_montserrat_24, C_TEXT);
    lv_obj_set_pos(history_title, 24, 20);
    history_range_text = label(history_popup, "", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_pos(history_range_text, 24, 53);
    lv_obj_t *history_close = lv_button_create(history_popup);
    lv_obj_set_pos(history_close, 716, 16); lv_obj_set_size(history_close, 60, 42);
    panel(history_close, C_PANEL_2, 14);
    lv_obj_t *history_close_text = label(history_close, "X", &lv_font_montserrat_18, C_TEXT);
    lv_obj_center(history_close_text);
    lv_obj_add_event_cb(history_close, history_close_event, LV_EVENT_CLICKED, NULL);
    history_chart = lv_chart_create(history_popup);
    lv_obj_set_pos(history_chart, 72, 86); lv_obj_set_size(history_chart, 704, 260);
    lv_obj_set_style_bg_color(history_chart, C_PANEL, 0);
    lv_obj_set_style_bg_opa(history_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(history_chart, 0, 0);
    lv_obj_set_style_radius(history_chart, 20, 0);
    lv_obj_set_style_pad_all(history_chart, 18, 0);
    lv_obj_set_style_line_color(history_chart, lv_color_hex(0x29414C), LV_PART_MAIN);
    lv_obj_set_style_line_width(history_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_size(history_chart, 0, 0, LV_PART_INDICATOR);
    lv_chart_set_type(history_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(history_chart, HISTORY_GRAPH_POINTS);
    lv_chart_set_div_line_count(history_chart, 5, 8);
    history_series = lv_chart_add_series(history_chart, C_CYAN, LV_CHART_AXIS_PRIMARY_Y);
    history_y_unit = label(history_popup, "ppm", &lv_font_montserrat_12, C_CYAN);
    lv_obj_set_pos(history_y_unit, 14, 67);
    static const int history_y_pos[] = {82, 145, 207, 269, 329};
    for (int i = 0; i < 5; ++i) {
        history_y_labels[i] = label(history_popup, "--", &lv_font_montserrat_12, C_MUTED);
        lv_obj_set_width(history_y_labels[i], 50);
        lv_obj_set_style_text_align(history_y_labels[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(history_y_labels[i], 14, history_y_pos[i]);
    }
    for (int i = 0; i < 3; ++i) {
        history_x_labels[i] = label(history_popup, i == 2 ? "now" : "--", &lv_font_montserrat_12, C_MUTED);
        lv_obj_set_width(history_x_labels[i], 80);
        lv_obj_set_style_text_align(history_x_labels[i], i == 0 ? LV_TEXT_ALIGN_LEFT : i == 1 ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_obj_set_pos(history_x_labels[0], 72, 352);
    lv_obj_set_pos(history_x_labels[1], 384, 352);
    lv_obj_set_pos(history_x_labels[2], 696, 352);
    for (int i = 0; i < 8; ++i) {
        lv_obj_t *range = lv_button_create(history_popup);
        history_range_buttons[i] = range;
        lv_obj_set_pos(range, 24 + i * 94, 385); lv_obj_set_size(range, 82, 52);
        panel(range, i == 0 ? C_CYAN : C_PANEL_2, 16);
        lv_obj_t *range_text = label(range, history_range_names[i], &lv_font_montserrat_14,
                                     i == 0 ? C_BG : C_TEXT);
        lv_obj_center(range_text);
        lv_obj_add_event_cb(range, history_range_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    splash = lv_obj_create(screen);
    lv_obj_set_pos(splash, 0, 0);
    lv_obj_set_size(splash, 800, 480);
    panel(splash, C_BG, 0);
    lv_obj_t *splash_brand = label(splash, "Airstation", &lv_font_montserrat_48, C_TEXT);
    lv_obj_align(splash_brand, LV_ALIGN_CENTER, 0, -46);
    lv_obj_t *splash_caption = label(splash, "Preparing your air quality station", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(splash_caption, LV_ALIGN_CENTER, 0, 12);
    lv_obj_t *splash_bar = lv_bar_create(splash);
    lv_obj_set_size(splash_bar, 360, 9);
    lv_obj_align(splash_bar, LV_ALIGN_CENTER, 0, 58);
    lv_bar_set_range(splash_bar, 0, 100);
    lv_bar_set_value(splash_bar, 4, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(splash_bar, C_PANEL_2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(splash_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(splash_bar, C_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(splash_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(splash_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(splash_bar, 16, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(splash_bar, C_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(splash_bar, LV_OPA_50, LV_PART_INDICATOR);
    lv_anim_t loading;
    lv_anim_init(&loading);
    lv_anim_set_var(&loading, splash_bar);
    lv_anim_set_values(&loading, 4, 100);
    lv_anim_set_duration(&loading, 5000);
    lv_anim_set_path_cb(&loading, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&loading, splash_progress);
    lv_anim_start(&loading);
    splash_started_us = esp_timer_get_time();
    bsp_display_brightness_set(brightness);
}

void air_ui_update(const sen66_data_t *d, bool connected, unsigned age)
{
    (void)age;
    char text[48];
    int minute = current_minute();
    time_t now = time(NULL);
    if (now > 1609459200) time_configured = true;
    format_time(text, sizeof(text), minute);
    lv_label_set_text(clock_label, text);
    /* Always start fully lit. Apply the schedule only when a configured
     * boundary is actually crossed while the device is running. */
    if (time_configured && minute != last_schedule_minute) {
        if (minute == display_on_minute) {
            backlight_on = true;
            bsp_display_brightness_set(brightness);
        } else if (minute == display_off_minute) {
            backlight_on = false;
            bsp_display_brightness_set(0);
        }
        last_schedule_minute = minute;
    }
    lv_obj_set_style_bg_color(status_dot, connected ? C_GREEN : C_RED, 0);
    lv_label_set_text(status_text, "Sensor");
    bool wifi_connected = wifi_manager_connected();
    lv_obj_set_style_bg_color(wifi_dot, wifi_connected ? C_GREEN : C_RED, 0);
    lv_label_set_text(wifi_text, "WiFi");
    if (!d || !sen66_data_valid(d)) return;
    if (splash && esp_timer_get_time() - splash_started_us >= 3000000) dismiss_splash();

    lv_color_t quality = quality_color(d);
    format_value(text, sizeof(text), d->co2, 0);
    animated_number_set(hero_value, text);
    lv_obj_set_style_bg_color(hero_badge, quality, 0);
    lv_label_set_text(lv_obj_get_child(hero_badge, 0), quality_text(d));
    if (!isnan(d->co2) && d->co2 > 1400) lv_label_set_text(hero_hint, "Open a window now");
    else if (!isnan(d->pm25) && d->pm25 > 35) lv_label_set_text(hero_hint, "Particle level is elevated");
    else if (!isnan(d->co2) && d->co2 > 1000) lv_label_set_text(hero_hint, "Fresh air would help");
    else lv_label_set_text(hero_hint, "Everything looks healthy");

    const float values[] = {d->pm25, d->voc, d->nox, d->temperature, d->humidity, d->pm10};
    const int decimals[] = {1, 0, 0, 1, 0, 1};
    for (int i = 0; i < 6; ++i) {
        format_value(text, sizeof(text), values[i], decimals[i]);
        animated_number_set(cards[i].value, text);
    }
    format_value(text, sizeof(text), d->co2, 0); animated_number_set(circle_co2, text);
    lv_label_set_text(circle_status, quality_text(d));
    lv_obj_set_style_text_color(circle_status, quality, 0);
    lv_obj_set_style_text_color(circle_co2, quality, 0);
    lv_obj_set_style_border_color(circle_core, quality, 0);
    lv_obj_set_style_shadow_color(circle_core, quality, 0);
    const float maximums[] = {50, 500, 500, 40, 100, 100};
    for (int i = 0; i < 6; ++i) {
        int percent = isnan(values[i]) ? 0 : (int)(values[i] / maximums[i] * 100);
        lv_arc_set_value(circle_arcs[i], LV_CLAMP(0, percent, 100));
        format_value(text, sizeof(text), values[i], decimals[i]);
        animated_number_set(circle_values[i], text);
    }
    const float particles[] = {d->pm1, d->pm25, d->pm4, d->pm10};
    for (int i = 0; i < 4; ++i) {
        format_value(text, sizeof(text), particles[i], 1);
        animated_number_set(particle_values[i], text);
    }
    lv_obj_set_style_bg_color(cards[0].bar, d->pm25 > 35 ? C_RED : d->pm25 > 15 ? C_YELLOW : C_CYAN, 0);
    lv_obj_set_style_bg_color(cards[1].bar, d->voc > 250 ? C_RED : d->voc > 100 ? C_YELLOW : C_GREEN, 0);
    lv_obj_set_style_bg_color(cards[2].bar, d->nox > 250 ? C_RED : d->nox > 100 ? C_YELLOW : C_GREEN, 0);

    if (history_tick++ % 60 == 0) {
        co2_recent[co2_recent_head] = d->co2;
        co2_recent_head = (co2_recent_head + 1) % 60;
        if (co2_recent_count < 60) ++co2_recent_count;
        lv_chart_set_next_value(chart, co2_series, isnan(d->co2) ? LV_CHART_POINT_NONE : (int32_t)d->co2);
        lv_chart_set_next_value(chart, pm_series, isnan(d->pm25) ? LV_CHART_POINT_NONE : (int32_t)d->pm25);
        lv_chart_refresh(chart);
        update_main_co2_scale();
        if (long_history) {
            const float samples[HISTORY_METRICS] = {
                d->co2, d->pm25, d->voc, d->nox, d->temperature, d->humidity, d->pm10
            };
            memcpy(&long_history[long_history_head * HISTORY_METRICS], samples, sizeof(samples));
            long_history_head = (long_history_head + 1) % HISTORY_MINUTES;
            if (long_history_count < HISTORY_MINUTES) ++long_history_count;
            if (!lv_obj_has_flag(history_popup, LV_OBJ_FLAG_HIDDEN)) refresh_history_graph();
        }
    }
}

void air_ui_set_display_enabled(bool enabled)
{
    backlight_on = enabled;
    bsp_display_brightness_set(enabled ? brightness : 0);
}

bool air_ui_display_enabled(void)
{
    return backlight_on;
}
