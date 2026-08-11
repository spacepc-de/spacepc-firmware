#include "recorder_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_settings.h"
#include "audio_recorder.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "lvgl.h"
#include "snore_classifier.h"
#include "sleep_engine.h"
#include "sleep_storage.h"
#include "time_zones.h"
#include "wifi_manager.h"

enum { PAGE_MONITOR, PAGE_SUMMARY, PAGE_HISTORY, PAGE_RECORDER, PAGE_SETTINGS, PAGE_COUNT };
enum { SUMMARY_TIMELINE_SEGMENTS = 96 };

static lv_obj_t *s_pages[PAGE_COUNT];
static lv_obj_t *s_record_state;
static lv_obj_t *s_record_time;
static lv_obj_t *s_record_level;
static lv_obj_t *s_record_meter[2];
static lv_obj_t *s_record_file;
static lv_obj_t *s_record_error;
static lv_obj_t *s_record_button;
static lv_obj_t *s_monitor_ring;
static lv_obj_t *s_monitor_threshold_ring;
static lv_obj_t *s_monitor_probability;
static lv_obj_t *s_monitor_probability_caption;
static lv_obj_t *s_monitor_state;
static lv_obj_t *s_monitor_mode;
static lv_obj_t *s_monitor_detail;
static lv_obj_t *s_monitor_status_dot;
static lv_obj_t *s_monitor_elapsed;
static lv_obj_t *s_monitor_events;
static lv_obj_t *s_monitor_start_button;
static lv_obj_t *s_monitor_record_button;
static lv_obj_t *s_summary_score;
static lv_obj_t *s_summary_note;
static lv_obj_t *s_summary_period;
static lv_obj_t *s_summary_monitored;
static lv_obj_t *s_summary_snoring;
static lv_obj_t *s_summary_events;
static lv_obj_t *s_summary_longest;
static lv_obj_t *s_summary_timeline_range;
static lv_obj_t *s_summary_timeline_bar;
static lv_obj_t *s_summary_timeline_segment[SUMMARY_TIMELINE_SEGMENTS];
static uint8_t s_summary_timeline_data[SLEEP_TIMELINE_BYTES];
static uint64_t s_summary_timeline_monitored_ms;
static uint64_t s_summary_timeline_started_unix_s;
static uint32_t s_summary_timeline_event_count;
static int s_summary_timeline_selection = -1;
static bool s_summary_timeline_available;
static lv_obj_t *s_history_empty;
static lv_obj_t *s_history_bar[7];
static lv_obj_t *s_history_value[7];
static lv_obj_t *s_history_date[7];
static lv_obj_t *s_wifi_status_value;
static lv_obj_t *s_time_status_value;
static lv_obj_t *s_wifi_dialog;
static lv_obj_t *s_wifi_ssid;
static lv_obj_t *s_wifi_password;
static lv_obj_t *s_wifi_keyboard;
static app_settings_t s_settings;
static lv_obj_t *s_wake_overlay;
static bool s_display_off;
static bool s_monitor_threshold_dragging;

static void open_page(int page);
static void refresh_summary(void);
static void refresh_history(void);

static void apply_threshold(uint8_t value)
{
    if (value < 5) value = 5;
    if (value > 90) value = 90;
    s_settings.start_probability = value;
    s_settings.end_probability = LV_MAX(5, value * 60 / 100);
    sleep_engine_set_event_thresholds(s_settings.start_probability, s_settings.end_probability);
}

static void monitor_threshold_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    uint8_t threshold = (uint8_t)lv_arc_get_value(lv_event_get_target(event));
    apply_threshold(threshold);

    if (code == LV_EVENT_VALUE_CHANGED) {
        char text[8];
        s_monitor_threshold_dragging = true;
        snprintf(text, sizeof(text), "%u%%", threshold);
        lv_label_set_text(s_monitor_probability, text);
        lv_label_set_text(s_monitor_probability_caption, "SNORE THRESHOLD");
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        sleep_engine_status_t sleep;
        char text[8];
        s_monitor_threshold_dragging = false;
        app_settings_save(&s_settings);
        sleep_engine_get_status(&sleep);
        if (sleep.model_available) {
            snprintf(text, sizeof(text), "%d%%", (int)(sleep.probability * 100 + .5f));
        } else {
            strlcpy(text, "--%", sizeof(text));
        }
        lv_label_set_text(s_monitor_probability, text);
        lv_label_set_text(s_monitor_probability_caption, "SNORE CONFIDENCE");
    }
}

static void wake_display_event(lv_event_t *event)
{
    (void)event;
    if (!s_display_off) return;
    s_display_off = false;
    esp_err_t panel_err = esp_lcd_panel_disp_on_off(bsp_display_get_panel_handle(), true);
    if (panel_err != ESP_OK) ESP_LOGW("sleep_ui", "Panel wake failed: %s", esp_err_to_name(panel_err));
    bsp_display_brightness_set(100);
    if (s_wake_overlay) {
        lv_obj_delete_async(s_wake_overlay);
        s_wake_overlay = NULL;
    }
}

static void display_off_event(lv_event_t *event)
{
    (void)event;
    if (s_display_off) return;
    s_wake_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_wake_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_wake_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_wake_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wake_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_wake_overlay, 0, 0);
    lv_obj_remove_flag(s_wake_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wake_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wake_overlay, wake_display_event, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_wake_overlay);
    s_display_off = true;
    bsp_display_backlight_off();
    esp_err_t panel_err = esp_lcd_panel_disp_on_off(bsp_display_get_panel_handle(), false);
    if (panel_err != ESP_OK) ESP_LOGW("sleep_ui", "Panel sleep failed: %s", esp_err_to_name(panel_err));
}

static void monitor_button_event(lv_event_t *event)
{
    sleep_engine_status_t status;
    sleep_engine_get_status(&status);
    bool record_audio = (bool)(intptr_t)lv_event_get_user_data(event);
    esp_err_t err = status.monitoring ? sleep_engine_set_monitoring(false) :
                                       sleep_engine_start_monitoring(record_audio);
    if (err != ESP_OK) ESP_LOGW("sleep_ui", "Monitoring toggle failed: %s", esp_err_to_name(err));
    else if (status.monitoring) open_page(PAGE_SUMMARY);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x121828), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x26304a), 0);
    lv_obj_set_style_radius(obj, 22, 0);
    lv_obj_set_style_pad_all(obj, 18, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void nav_event(lv_event_t *event)
{
    open_page((int)(intptr_t)lv_event_get_user_data(event));
}

static void add_header(lv_obj_t *screen, int active)
{
    lv_obj_t *brand = label(screen, "SLEEP BUDDY", &lv_font_montserrat_18, 0x9f96ff);
    lv_obj_set_pos(brand, 26, 18);
    lv_obj_t *power = lv_button_create(screen);
    lv_obj_set_size(power, 52, 38);
    lv_obj_set_pos(power, 158, 10);
    lv_obj_set_style_radius(power, 14, 0);
    lv_obj_set_style_shadow_width(power, 0, 0);
    lv_obj_set_style_bg_color(power, lv_color_hex(0x252c40), 0);
    lv_obj_add_event_cb(power, display_off_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *power_text = label(power, "OFF", &lv_font_montserrat_14, 0xbfc6da);
    lv_obj_center(power_text);
    static const int pages[] = {PAGE_MONITOR, PAGE_SUMMARY, PAGE_HISTORY, PAGE_SETTINGS};
    static const char *names[] = {"Monitor", "Summary", "History", "Settings"};
    for (int i = 0; i < 4; ++i) {
        int page = pages[i];
        lv_obj_t *button = lv_button_create(screen);
        lv_obj_set_size(button, 110, 38);
        lv_obj_set_pos(button, 258 + i * 124, 10);
        lv_obj_set_style_radius(button, 14, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(page == active ? 0x7568ef : 0x111625), 0);
        lv_obj_add_event_cb(button, nav_event, LV_EVENT_CLICKED, (void *)(intptr_t)page);
        lv_obj_t *text = label(button, names[i], &lv_font_montserrat_14, 0xffffff);
        lv_obj_center(text);
    }
}

static lv_obj_t *make_screen(int active, const char *title, const char *subtitle)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x070b10), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    add_header(screen, active);
    lv_obj_t *heading = label(screen, title, &lv_font_montserrat_32, 0xf6f7ff);
    lv_obj_set_pos(heading, 28, 67);
    if (subtitle && subtitle[0]) {
        lv_obj_t *sub = label(screen, subtitle, &lv_font_montserrat_14, 0x838ba3);
        lv_obj_set_pos(sub, 30, 107);
    }
    return screen;
}

static void create_monitor(void)
{
    lv_obj_t *screen = make_screen(PAGE_MONITOR, "Tonight", "");

    lv_obj_t *hero = card(screen, 28, 143, 326, 306);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x151b31), 0);
    lv_obj_set_style_bg_grad_color(hero, lv_color_hex(0x0d1220), 0);
    lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(hero, lv_color_hex(0x514a8e), 0);

    lv_obj_t *halo = lv_obj_create(hero);
    lv_obj_set_size(halo, 238, 238);
    lv_obj_align(halo, LV_ALIGN_TOP_MID, 0, -7);
    lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(halo, lv_color_hex(0x7568ef), 0);
    lv_obj_set_style_bg_opa(halo, LV_OPA_10, 0);
    lv_obj_set_style_border_width(halo, 1, 0);
    lv_obj_set_style_border_color(halo, lv_color_hex(0x665cb8), 0);
    lv_obj_set_style_border_opa(halo, LV_OPA_40, 0);
    lv_obj_remove_flag(halo, LV_OBJ_FLAG_SCROLLABLE);

    s_monitor_ring = lv_arc_create(hero);
    lv_obj_set_size(s_monitor_ring, 210, 210);
    lv_obj_align(s_monitor_ring, LV_ALIGN_TOP_MID, 0, 7);
    lv_arc_set_rotation(s_monitor_ring, 135);
    lv_arc_set_bg_angles(s_monitor_ring, 0, 270);
    lv_arc_set_value(s_monitor_ring, 0);
    lv_obj_remove_style(s_monitor_ring, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_monitor_ring, 13, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_monitor_ring, 13, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_monitor_ring, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_monitor_ring, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_monitor_ring, lv_color_hex(0x2a3048), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_monitor_ring, lv_color_hex(0x8d80ff), LV_PART_INDICATOR);

    s_monitor_threshold_ring = lv_arc_create(hero);
    lv_obj_set_size(s_monitor_threshold_ring, 232, 232);
    lv_obj_align(s_monitor_threshold_ring, LV_ALIGN_TOP_MID, 0, -4);
    lv_arc_set_rotation(s_monitor_threshold_ring, 135);
    lv_arc_set_bg_angles(s_monitor_threshold_ring, 0, 270);
    lv_arc_set_range(s_monitor_threshold_ring, 5, 90);
    lv_arc_set_value(s_monitor_threshold_ring, s_settings.start_probability);
    lv_obj_set_style_arc_opa(s_monitor_threshold_ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_monitor_threshold_ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_monitor_threshold_ring, lv_color_hex(0xff6b8a), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_monitor_threshold_ring, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_monitor_threshold_ring, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_monitor_threshold_ring, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_monitor_threshold_ring, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(s_monitor_threshold_ring, monitor_threshold_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_monitor_threshold_ring, monitor_threshold_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_monitor_threshold_ring, monitor_threshold_event, LV_EVENT_PRESS_LOST, NULL);

    s_monitor_probability = label(hero, "--%", &lv_font_montserrat_48, 0xf8f9ff);
    lv_obj_align(s_monitor_probability, LV_ALIGN_TOP_MID, 0, 79);
    s_monitor_probability_caption = label(hero, "SNORE CONFIDENCE", &lv_font_montserrat_14, 0x9aa2ba);
    lv_obj_align(s_monitor_probability_caption, LV_ALIGN_TOP_MID, 0, 140);

    lv_obj_t *divider = lv_obj_create(hero);
    lv_obj_set_size(divider, 278, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x333a55), 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);
    s_monitor_elapsed = label(hero, "00:00:00", &lv_font_montserrat_18, 0xf4f6ff);
    lv_obj_align(s_monitor_elapsed, LV_ALIGN_BOTTOM_LEFT, 20, -25);
    lv_obj_t *elapsed_caption = label(hero, "ELAPSED", &lv_font_montserrat_14, 0x747e98);
    lv_obj_align(elapsed_caption, LV_ALIGN_BOTTOM_LEFT, 20, -3);
    s_monitor_events = label(hero, "0", &lv_font_montserrat_18, 0xf4f6ff);
    lv_obj_align(s_monitor_events, LV_ALIGN_BOTTOM_RIGHT, -20, -25);
    lv_obj_t *events_caption = label(hero, "EVENTS", &lv_font_montserrat_14, 0x747e98);
    lv_obj_align(events_caption, LV_ALIGN_BOTTOM_RIGHT, -20, -3);

    lv_obj_t *status = card(screen, 376, 143, 396, 170);
    lv_obj_set_style_bg_color(status, lv_color_hex(0x111827), 0);
    s_monitor_status_dot = lv_obj_create(status);
    lv_obj_set_size(s_monitor_status_dot, 12, 12);
    lv_obj_set_style_radius(s_monitor_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_monitor_status_dot, lv_color_hex(0x8d80ff), 0);
    lv_obj_set_style_border_width(s_monitor_status_dot, 0, 0);
    lv_obj_set_pos(s_monitor_status_dot, 2, 5);
    s_monitor_mode = label(status, "READY", &lv_font_montserrat_14, 0x9f96ff);
    lv_obj_set_pos(s_monitor_mode, 24, 1);
    s_monitor_state = label(status, "Ready to monitor", &lv_font_montserrat_24, 0xf4f6ff);
    lv_obj_set_pos(s_monitor_state, 2, 31);
    s_monitor_detail = label(status, "Choose analysis or analysis + recording.", &lv_font_montserrat_14, 0x8e97af);
    lv_obj_set_pos(s_monitor_detail, 2, 70);

    lv_obj_t *actions = card(screen, 376, 331, 396, 118);
    lv_obj_set_style_bg_color(actions, lv_color_hex(0x15152a), 0);
    lv_obj_set_style_border_color(actions, lv_color_hex(0x433d75), 0);
    lv_obj_t *action_title = label(actions, "START A NIGHT", &lv_font_montserrat_14, 0x9aa2ba);
    lv_obj_set_pos(action_title, 2, -2);

    s_monitor_start_button = lv_button_create(actions);
    lv_obj_set_size(s_monitor_start_button, 170, 48);
    lv_obj_align(s_monitor_start_button, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_radius(s_monitor_start_button, 15, 0);
    lv_obj_set_style_bg_color(s_monitor_start_button, lv_color_hex(0x2fbf91), 0);
    lv_obj_set_style_shadow_width(s_monitor_start_button, 0, 0);
    lv_obj_add_event_cb(s_monitor_start_button, monitor_button_event, LV_EVENT_CLICKED, (void *)0);
    lv_obj_t *start_text = label(s_monitor_start_button, "START", &lv_font_montserrat_14, 0xffffff);
    lv_obj_center(start_text);

    s_monitor_record_button = lv_button_create(actions);
    lv_obj_set_size(s_monitor_record_button, 178, 48);
    lv_obj_align(s_monitor_record_button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(s_monitor_record_button, 15, 0);
    lv_obj_set_style_bg_color(s_monitor_record_button, lv_color_hex(0xe0526a), 0);
    lv_obj_set_style_shadow_width(s_monitor_record_button, 0, 0);
    lv_obj_add_event_cb(s_monitor_record_button, monitor_button_event, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *record_text = label(s_monitor_record_button, "START + RECORD", &lv_font_montserrat_14, 0xffffff);
    lv_obj_center(record_text);
    s_pages[PAGE_MONITOR] = screen;
}

static void format_timeline_position(char *text, size_t size, uint64_t relative_ms)
{
    if (s_summary_timeline_started_unix_s) {
        time_t timestamp = (time_t)(s_summary_timeline_started_unix_s + relative_ms / 1000U);
        struct tm local;
        localtime_r(&timestamp, &local);
        strftime(text, size, "%H:%M", &local);
        return;
    }
    uint64_t minutes = relative_ms / 60000U;
    uint32_t hours = (uint32_t)(minutes / 60U);
    uint32_t remaining_minutes = (uint32_t)(minutes % 60U);
    if (hours > 24U) hours = 24U;
    snprintf(text, size, "+%02u:%02u", (unsigned)hours, (unsigned)remaining_minutes);
}

static void show_timeline_selection(int segment)
{
    if (!s_summary_timeline_range) return;
    if (!s_summary_timeline_available || !s_summary_timeline_monitored_ms) {
        lv_label_set_text(s_summary_timeline_range, "NO TIMELINE AVAILABLE");
        lv_obj_set_style_text_color(s_summary_timeline_range, lv_color_hex(0x8c94aa), 0);
        return;
    }
    if (segment < 0 || segment >= SUMMARY_TIMELINE_SEGMENTS) {
        lv_label_set_text(s_summary_timeline_range,
                          s_summary_timeline_event_count ? "TAP THE TIMELINE FOR TIME" : "QUIET");
        lv_obj_set_style_text_color(s_summary_timeline_range,
                                    lv_color_hex(s_summary_timeline_event_count ? 0x8c94aa : 0x63e2c6), 0);
        return;
    }

    uint64_t first_ms = (uint64_t)segment * s_summary_timeline_monitored_ms /
                        SUMMARY_TIMELINE_SEGMENTS;
    uint64_t end_ms = (uint64_t)(segment + 1) * s_summary_timeline_monitored_ms /
                      SUMMARY_TIMELINE_SEGMENTS;
    uint64_t first_minute = first_ms / 60000U;
    uint64_t last_minute = end_ms ? (end_ms - 1U) / 60000U : first_minute;
    if (last_minute >= SLEEP_TIMELINE_MINUTES) last_minute = SLEEP_TIMELINE_MINUTES - 1U;

    int selected_minute = -1;
    uint64_t middle_minute = ((first_ms + end_ms) / 2U) / 60000U;
    uint64_t best_distance = UINT64_MAX;
    for (uint64_t minute = first_minute;
         minute <= last_minute && minute < SLEEP_TIMELINE_MINUTES; ++minute) {
        if (!sleep_session_timeline_minute(s_summary_timeline_data, (uint16_t)minute)) continue;
        uint64_t distance = minute > middle_minute ? minute - middle_minute : middle_minute - minute;
        if (distance < best_distance) {
            best_distance = distance;
            selected_minute = (int)minute;
        }
    }

    bool snoring = selected_minute >= 0;
    uint64_t selected_start_ms = first_ms;
    uint64_t selected_end_ms = end_ms;
    if (snoring) {
        int run_start = selected_minute;
        int run_end = selected_minute;
        while (run_start > 0 && sleep_session_timeline_minute(
                   s_summary_timeline_data, (uint16_t)(run_start - 1))) --run_start;
        while (run_end + 1 < SLEEP_TIMELINE_MINUTES && sleep_session_timeline_minute(
                   s_summary_timeline_data, (uint16_t)(run_end + 1))) ++run_end;
        selected_start_ms = (uint64_t)run_start * 60000U;
        selected_end_ms = (uint64_t)(run_end + 1) * 60000U;
        if (selected_end_ms > s_summary_timeline_monitored_ms) {
            selected_end_ms = s_summary_timeline_monitored_ms;
        }
    }

    char start[12];
    char end[12];
    char result[48];
    format_timeline_position(start, sizeof(start), selected_start_ms);
    format_timeline_position(end, sizeof(end), selected_end_ms);
    snprintf(result, sizeof(result), "%s  %s - %s", snoring ? "SNORING" : "QUIET", start, end);
    lv_label_set_text(s_summary_timeline_range, result);
    lv_obj_set_style_text_color(s_summary_timeline_range,
                                lv_color_hex(snoring ? 0xff7185 : 0x63e2c6), 0);
}

static void summary_timeline_event(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t point;
    lv_area_t area;
    lv_indev_get_point(indev, &point);
    lv_obj_get_coords(lv_event_get_target(event), &area);
    int width = area.x2 - area.x1 + 1;
    int relative_x = LV_CLAMP(0, point.x - area.x1, width - 1);
    s_summary_timeline_selection = relative_x * SUMMARY_TIMELINE_SEGMENTS / width;
    show_timeline_selection(s_summary_timeline_selection);
}

static void create_summary(void)
{
    lv_obj_t *screen = make_screen(PAGE_SUMMARY, "Good morning", "Last completed monitoring session");
    s_summary_period = label(screen, "Time unavailable - connect WiFi for NTP",
                             &lv_font_montserrat_14, 0x838ba3);
    lv_obj_set_width(s_summary_period, 360);
    lv_obj_set_style_text_align(s_summary_period, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_summary_period, 410, 107);
    lv_obj_t *score = card(screen, 28, 143, 232, 306);
    lv_obj_t *small = label(score, "ACOUSTIC SCORE", &lv_font_montserrat_14, 0x8c94aa);
    lv_obj_align(small, LV_ALIGN_TOP_MID, 0, 3);
    s_summary_score = label(score, "--", &lv_font_montserrat_48, 0x63e2c6);
    lv_obj_align(s_summary_score, LV_ALIGN_CENTER, 0, -35);
    s_summary_note = label(score, "Complete a monitoring\nsession to see results", &lv_font_montserrat_14, 0x8c94aa);
    lv_obj_set_style_text_align(s_summary_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_summary_note, 190);
    lv_obj_align(s_summary_note, LV_ALIGN_BOTTOM_MID, 0, -18);
    const char *values[] = {"Monitored", "Snoring detected", "Events", "Longest phase"};
    const char *empty[] = {"-- h -- min", "-- min", "--", "-- min"};
    lv_obj_t **outputs[] = {&s_summary_monitored, &s_summary_snoring, &s_summary_events, &s_summary_longest};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *item = card(screen, 280 + (i % 2) * 246, 143 + (i / 2) * 118, 226, 108);
        lv_obj_t *a = label(item, values[i], &lv_font_montserrat_14, 0x8c94aa);
        lv_obj_set_pos(a, 1, 2);
        *outputs[i] = label(item, empty[i], &lv_font_montserrat_24, 0xf5f7ff);
        lv_obj_set_pos(*outputs[i], 1, 39);
    }

    lv_obj_t *timeline = card(screen, 280, 379, 492, 70);
    lv_obj_set_style_pad_all(timeline, 10, 0);
    lv_obj_t *timeline_title = label(timeline, "NIGHT TIMELINE", &lv_font_montserrat_14, 0x8c94aa);
    lv_obj_set_pos(timeline_title, 2, -1);
    s_summary_timeline_range = label(timeline, "TAP THE TIMELINE FOR TIME", &lv_font_montserrat_14, 0x8c94aa);
    lv_obj_set_width(s_summary_timeline_range, 300);
    lv_obj_set_style_text_align(s_summary_timeline_range, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_summary_timeline_range, LV_ALIGN_TOP_RIGHT, -2, -1);
    s_summary_timeline_bar = lv_obj_create(timeline);
    lv_obj_set_pos(s_summary_timeline_bar, 2, 28);
    lv_obj_set_size(s_summary_timeline_bar, 468, 14);
    lv_obj_set_style_bg_color(s_summary_timeline_bar, lv_color_hex(0x285447), 0);
    lv_obj_set_style_bg_opa(s_summary_timeline_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_summary_timeline_bar, 0, 0);
    lv_obj_set_style_radius(s_summary_timeline_bar, 7, 0);
    lv_obj_set_style_pad_all(s_summary_timeline_bar, 0, 0);
    lv_obj_remove_flag(s_summary_timeline_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_summary_timeline_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_summary_timeline_bar, 8);
    lv_obj_add_event_cb(s_summary_timeline_bar, summary_timeline_event, LV_EVENT_CLICKED, NULL);
    for (int i = 0; i < SUMMARY_TIMELINE_SEGMENTS; ++i) {
        int x0 = i * 468 / SUMMARY_TIMELINE_SEGMENTS;
        int x1 = (i + 1) * 468 / SUMMARY_TIMELINE_SEGMENTS;
        s_summary_timeline_segment[i] = lv_obj_create(s_summary_timeline_bar);
        lv_obj_set_pos(s_summary_timeline_segment[i], x0, 0);
        lv_obj_set_size(s_summary_timeline_segment[i], x1 - x0, 14);
        lv_obj_set_style_bg_color(s_summary_timeline_segment[i], lv_color_hex(0xff536e), 0);
        lv_obj_set_style_bg_opa(s_summary_timeline_segment[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_summary_timeline_segment[i], 0, 0);
        lv_obj_set_style_radius(s_summary_timeline_segment[i], 0, 0);
        lv_obj_set_style_pad_all(s_summary_timeline_segment[i], 0, 0);
        lv_obj_remove_flag(s_summary_timeline_segment[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_summary_timeline_segment[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_summary_timeline_segment[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_pages[PAGE_SUMMARY] = screen;
}

static void create_history(void)
{
    lv_obj_t *screen = make_screen(PAGE_HISTORY, "Seven-night trend", "Objective acoustic statistics - no health interpretation");
    lv_obj_t *chart_card = card(screen, 28, 143, 744, 306);
    lv_obj_t *metric = label(chart_card, "SNORING MINUTES", &lv_font_montserrat_14, 0x8c94aa);
    lv_obj_set_pos(metric, 2, 0);
    s_history_empty = label(chart_card, "NO COMPLETED NIGHTS YET", &lv_font_montserrat_24, 0x9b94ff);
    lv_obj_align(s_history_empty, LV_ALIGN_CENTER, 0, 0);
    for (int i = 0; i < 7; ++i) {
        int x = 35 + i * 96;
        s_history_bar[i] = lv_obj_create(chart_card);
        lv_obj_set_size(s_history_bar[i], 48, 6);
        lv_obj_set_pos(s_history_bar[i], x, 218);
        lv_obj_set_style_radius(s_history_bar[i], 12, 0);
        lv_obj_set_style_border_width(s_history_bar[i], 0, 0);
        lv_obj_set_style_bg_color(s_history_bar[i], lv_color_hex(0x7568ef), 0);
        lv_obj_remove_flag(s_history_bar[i], LV_OBJ_FLAG_SCROLLABLE);
        s_history_value[i] = label(chart_card, "", &lv_font_montserrat_14, 0xc9c5ff);
        lv_obj_set_width(s_history_value[i], 70);
        lv_obj_set_style_text_align(s_history_value[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_history_value[i], x - 11, 196);
        s_history_date[i] = label(chart_card, "", &lv_font_montserrat_14, 0x7f889f);
        lv_obj_set_width(s_history_date[i], 78);
        lv_obj_set_style_text_align(s_history_date[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_history_date[i], x - 15, 240);
        lv_obj_add_flag(s_history_bar[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_pages[PAGE_HISTORY] = screen;
}

static void record_button_event(lv_event_t *event)
{
    (void)event;
    recorder_status_t status;
    audio_recorder_get_status(&status);
    esp_err_t err = status.recording ? audio_recorder_stop() : audio_recorder_start();
    if (err != ESP_OK) ESP_LOGW("sleep_ui", "Recorder toggle failed: %s", esp_err_to_name(err));
}

static void create_recorder(void)
{
    lv_obj_t *screen = make_screen(PAGE_RECORDER, "Dataset recorder", "32 kHz / 16-bit / stereo WAV - model-native hardware samples");
    lv_obj_t *panel = card(screen, 28, 143, 744, 306);
    s_record_state = label(panel, "INITIALIZING", &lv_font_montserrat_18, 0x9b94ff);
    lv_obj_set_pos(s_record_state, 8, 2);
    s_record_time = label(panel, "00:00", &lv_font_montserrat_48, 0xf5f7ff);
    lv_obj_align(s_record_time, LV_ALIGN_TOP_MID, 0, 20);
    for (int i = 0; i < 2; ++i) {
        s_record_meter[i] = lv_bar_create(panel);
        lv_obj_set_size(s_record_meter[i], 650, 14);
        lv_obj_align(s_record_meter[i], LV_ALIGN_TOP_MID, 0, 102 + i * 24);
        lv_obj_set_style_radius(s_record_meter[i], 9, LV_PART_MAIN);
        lv_obj_set_style_radius(s_record_meter[i], 9, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_record_meter[i], lv_color_hex(0x252d42), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_record_meter[i], lv_color_hex(i ? 0x8e82ff : 0x5bdac1), LV_PART_INDICATOR);
    }
    s_record_level = label(panel, "MIC 1  -96.0 dBFS          MIC 2  -96.0 dBFS", &lv_font_montserrat_14, 0xa6aec2);
    lv_obj_align(s_record_level, LV_ALIGN_TOP_MID, 0, 154);
    s_record_button = lv_button_create(panel);
    lv_obj_set_size(s_record_button, 250, 58);
    lv_obj_align(s_record_button, LV_ALIGN_BOTTOM_MID, 0, -23);
    lv_obj_set_style_radius(s_record_button, 19, 0);
    lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0x7568ef), 0);
    lv_obj_add_event_cb(s_record_button, record_button_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *button_text = label(s_record_button, "START RECORDING", &lv_font_montserrat_18, 0xffffff);
    lv_obj_center(button_text);
    s_record_file = label(panel, "", &lv_font_montserrat_14, 0x7e879d);
    lv_obj_align(s_record_file, LV_ALIGN_BOTTOM_LEFT, 2, 2);
    s_record_error = label(panel, "", &lv_font_montserrat_14, 0xff7185);
    lv_obj_align(s_record_error, LV_ALIGN_BOTTOM_RIGHT, -2, 2);
    s_pages[PAGE_RECORDER] = screen;
}

static void wifi_dialog_close(void)
{
    if (!s_wifi_dialog) return;
    lv_obj_t *dialog = s_wifi_dialog;
    s_wifi_dialog = NULL;
    s_wifi_ssid = NULL;
    s_wifi_password = NULL;
    s_wifi_keyboard = NULL;
    lv_obj_delete_async(dialog);
}

static void wifi_cancel_event(lv_event_t *event)
{
    (void)event;
    wifi_dialog_close();
}

static void wifi_keyboard_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_keyboard_set_textarea(s_wifi_keyboard, NULL);
        lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wifi_field_event(lv_event_t *event)
{
    if (!s_wifi_keyboard) return;
    lv_keyboard_set_textarea(s_wifi_keyboard, lv_event_get_target(event));
    lv_obj_remove_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wifi_keyboard);
}

static void wifi_save_event(lv_event_t *event)
{
    (void)event;
    const char *ssid = lv_textarea_get_text(s_wifi_ssid);
    const char *password = lv_textarea_get_text(s_wifi_password);
    strlcpy(s_settings.wifi_ssid, ssid, sizeof(s_settings.wifi_ssid));
    strlcpy(s_settings.wifi_password, password, sizeof(s_settings.wifi_password));
    app_settings_save(&s_settings);
    esp_err_t err = wifi_manager_connect(s_settings.wifi_ssid, s_settings.wifi_password);
    if (err != ESP_OK) ESP_LOGW("sleep_ui", "WiFi connect failed: %s", esp_err_to_name(err));
    wifi_dialog_close();
}

static void wifi_open_event(lv_event_t *event)
{
    (void)event;
    if (s_wifi_dialog) return;
    s_wifi_dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_wifi_dialog, 0, 0);
    lv_obj_set_size(s_wifi_dialog, 800, 480);
    lv_obj_set_style_bg_color(s_wifi_dialog, lv_color_hex(0x070b10), 0);
    lv_obj_set_style_bg_opa(s_wifi_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_dialog, 0, 0);
    lv_obj_set_style_radius(s_wifi_dialog, 0, 0);
    lv_obj_remove_flag(s_wifi_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = label(s_wifi_dialog, "WiFi setup", &lv_font_montserrat_24, 0xf6f7ff);
    lv_obj_set_pos(title, 28, 18);
    lv_obj_t *hint = label(s_wifi_dialog, "Credentials stay locally on Sleep Buddy", &lv_font_montserrat_14, 0x838ba3);
    lv_obj_set_pos(hint, 28, 52);

    s_wifi_ssid = lv_textarea_create(s_wifi_dialog);
    s_wifi_password = lv_textarea_create(s_wifi_dialog);
    lv_obj_t *fields[] = {s_wifi_ssid, s_wifi_password};
    const char *placeholders[] = {"Network name (SSID)", "WiFi password"};
    for (int i = 0; i < 2; ++i) {
        lv_obj_set_pos(fields[i], 28, 92 + i * 62);
        lv_obj_set_size(fields[i], 510, 50);
        lv_textarea_set_one_line(fields[i], true);
        lv_textarea_set_placeholder_text(fields[i], placeholders[i]);
        lv_obj_set_style_bg_color(fields[i], lv_color_hex(0x121828), 0);
        lv_obj_set_style_text_color(fields[i], lv_color_hex(0xf5f7ff), 0);
        lv_obj_set_style_border_color(fields[i], lv_color_hex(0x3b4563), 0);
        lv_obj_add_event_cb(fields[i], wifi_field_event, LV_EVENT_CLICKED, NULL);
    }
    lv_textarea_set_text(s_wifi_ssid, s_settings.wifi_ssid);
    lv_textarea_set_text(s_wifi_password, s_settings.wifi_password);
    lv_textarea_set_password_mode(s_wifi_password, true);

    lv_obj_t *cancel = lv_button_create(s_wifi_dialog);
    lv_obj_set_pos(cancel, 570, 92); lv_obj_set_size(cancel, 200, 50);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x252d42), 0);
    lv_obj_set_style_radius(cancel, 15, 0);
    lv_obj_add_event_cb(cancel, wifi_cancel_event, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(cancel, "CANCEL", &lv_font_montserrat_14, 0xffffff));
    lv_obj_t *save = lv_button_create(s_wifi_dialog);
    lv_obj_set_pos(save, 570, 154); lv_obj_set_size(save, 200, 50);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x63e2c6), 0);
    lv_obj_set_style_radius(save, 15, 0);
    lv_obj_add_event_cb(save, wifi_save_event, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(save, "SAVE & CONNECT", &lv_font_montserrat_14, 0x07110f));

    s_wifi_keyboard = lv_keyboard_create(s_wifi_dialog);
    lv_obj_set_size(s_wifi_keyboard, 800, 260);
    lv_obj_align(s_wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_wifi_keyboard, wifi_keyboard_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_wifi_keyboard, wifi_keyboard_event, LV_EVENT_CANCEL, NULL);
    lv_keyboard_set_textarea(s_wifi_keyboard, NULL);
    lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void timezone_event(lv_event_t *event)
{
    uint32_t selected = lv_dropdown_get_selected(lv_event_get_target(event));
    if (selected >= time_zones_count()) selected = 0;
    s_settings.timezone_index = selected;
    time_zones_apply(s_settings.timezone_index);
    app_settings_save(&s_settings);
}

static void create_settings(void)
{
    lv_obj_t *screen = make_screen(PAGE_SETTINGS, "Settings", "Stored locally - network and time");
    const int card_width = 366;
    const int control_width = 330;
    lv_obj_t *wifi = card(screen, 28, 143, card_width, 144);
    lv_obj_t *wt = label(wifi, "WIFI & NTP", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(wt, 0, 0);
    s_wifi_status_value = label(wifi, "Not connected", &lv_font_montserrat_18, 0xffc46b);
    lv_obj_set_pos(s_wifi_status_value, 0, 32);
    lv_obj_t *wifi_button = lv_button_create(wifi);
    lv_obj_set_size(wifi_button, control_width, 40);
    lv_obj_align(wifi_button, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(wifi_button, 12, 0);
    lv_obj_set_style_bg_color(wifi_button, lv_color_hex(0x7568ef), 0);
    lv_obj_add_event_cb(wifi_button, wifi_open_event, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(wifi_button, "CONFIGURE", &lv_font_montserrat_14, 0xffffff));

    lv_obj_t *timezone = card(screen, 406, 143, card_width, 144);
    lv_obj_t *zt = label(timezone, "TIME ZONE", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(zt, 0, 0);
    lv_obj_t *zone_dropdown = lv_dropdown_create(timezone);
    lv_dropdown_set_options(zone_dropdown, time_zones_dropdown_options());
    lv_dropdown_set_selected(zone_dropdown, s_settings.timezone_index);
    lv_obj_set_size(zone_dropdown, control_width, 48);
    lv_obj_align(zone_dropdown, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(zone_dropdown, lv_color_hex(0x252d42), 0);
    lv_obj_set_style_text_color(zone_dropdown, lv_color_hex(0xf5f7ff), 0);
    lv_obj_set_style_border_width(zone_dropdown, 0, 0);
    lv_obj_add_event_cb(zone_dropdown, timezone_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *clock = card(screen, 28, 305, 744, 110);
    lv_obj_t *tt = label(clock, "INTERNET TIME", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(tt, 0, 0);
    s_time_status_value = label(clock, "Waiting for NTP", &lv_font_montserrat_18, 0xffc46b);
    lv_obj_set_width(s_time_status_value, 300);
    lv_obj_set_style_text_align(s_time_status_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_time_status_value, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *clock_note = label(clock, "Automatic time for summaries and seven-night history",
                                 &lv_font_montserrat_14, 0x7f889f);
    lv_obj_set_pos(clock_note, 0, 44);
    s_pages[PAGE_SETTINGS] = screen;
}

static void open_page(int page)
{
    if (page < 0 || page >= PAGE_COUNT || !s_pages[page]) return;
    if (page == PAGE_SUMMARY) refresh_summary();
    if (page == PAGE_HISTORY) refresh_history();
    lv_screen_load_anim(s_pages[page], LV_SCR_LOAD_ANIM_FADE_IN, 180, 0, false);
}

static void format_monitored(char *text, size_t size, uint64_t milliseconds)
{
    uint64_t minutes = milliseconds / 60000;
    if (minutes >= 60) snprintf(text, size, "%llu h %02llu min",
                                (unsigned long long)(minutes / 60),
                                (unsigned long long)(minutes % 60));
    else snprintf(text, size, "%llu min", (unsigned long long)minutes);
}

static void format_phase(char *text, size_t size, uint64_t milliseconds)
{
    uint64_t seconds = milliseconds / 1000;
    if (seconds >= 3600) snprintf(text, size, "%llu h %02llu min",
                                  (unsigned long long)(seconds / 3600),
                                  (unsigned long long)((seconds / 60) % 60));
    else if (seconds >= 60) snprintf(text, size, "%llu min %02llu s",
                                     (unsigned long long)(seconds / 60),
                                     (unsigned long long)(seconds % 60));
    else snprintf(text, size, "%llu s", (unsigned long long)seconds);
}

static void refresh_summary(void)
{
    sleep_summary_t summary;
    if (!sleep_engine_get_last_summary(&summary)) return;
    uint64_t started_unix_s = 0;
    uint64_t ended_unix_s = 0;
    bool has_times = sleep_engine_get_last_times(&started_unix_s, &ended_unix_s);
    char text[96];
    snprintf(text, sizeof(text), "%u", summary.acoustic_score);
    lv_label_set_text(s_summary_score, text);
    uint32_t score_color = summary.acoustic_score >= 85 ? 0x63e2c6 :
                           summary.acoustic_score >= 65 ? 0xf0c66c : 0xff7185;
    lv_obj_set_style_text_color(s_summary_score, lv_color_hex(score_color), 0);
    format_monitored(text, sizeof(text), summary.monitored_ms);
    lv_label_set_text(s_summary_monitored, text);
    format_phase(text, sizeof(text), summary.snore_ms);
    lv_label_set_text(s_summary_snoring, text);
    snprintf(text, sizeof(text), "%lu", (unsigned long)summary.event_count);
    lv_label_set_text(s_summary_events, text);
    format_phase(text, sizeof(text), summary.longest_event_ms);
    lv_label_set_text(s_summary_longest, text);
    if (summary.event_count) {
        snprintf(text, sizeof(text), "%.1f%% of monitored time\nPeak confidence %.0f%%",
                 summary.snore_percent, summary.strongest_probability * 100.0f);
    } else {
        snprintf(text, sizeof(text), "No snoring detected\nAcoustic estimate only");
    }
    lv_label_set_text(s_summary_note, text);

    uint8_t timeline[SLEEP_TIMELINE_BYTES];
    bool has_timeline = sleep_engine_get_last_timeline(timeline);
    bool timeline_changed = summary.monitored_ms != s_summary_timeline_monitored_ms ||
                            started_unix_s != s_summary_timeline_started_unix_s ||
                            summary.event_count != s_summary_timeline_event_count;
    if (timeline_changed) s_summary_timeline_selection = -1;
    s_summary_timeline_available = has_timeline;
    s_summary_timeline_monitored_ms = summary.monitored_ms;
    s_summary_timeline_started_unix_s = started_unix_s;
    s_summary_timeline_event_count = summary.event_count;
    if (has_timeline) memcpy(s_summary_timeline_data, timeline, sizeof(timeline));
    else memset(s_summary_timeline_data, 0, sizeof(s_summary_timeline_data));
    for (int i = 0; i < SUMMARY_TIMELINE_SEGMENTS; ++i) {
        bool snoring = false;
        if (has_timeline && summary.monitored_ms) {
            uint64_t first_ms = (uint64_t)i * summary.monitored_ms / SUMMARY_TIMELINE_SEGMENTS;
            uint64_t end_ms = (uint64_t)(i + 1) * summary.monitored_ms / SUMMARY_TIMELINE_SEGMENTS;
            uint64_t first_minute = first_ms / 60000U;
            uint64_t last_minute = end_ms ? (end_ms - 1U) / 60000U : first_minute;
            if (last_minute >= SLEEP_TIMELINE_MINUTES) last_minute = SLEEP_TIMELINE_MINUTES - 1U;
            for (uint64_t minute = first_minute;
                 minute <= last_minute && minute < SLEEP_TIMELINE_MINUTES; ++minute) {
                if (sleep_session_timeline_minute(timeline, (uint16_t)minute)) {
                    snoring = true;
                    break;
                }
            }
        }
        if (snoring) lv_obj_remove_flag(s_summary_timeline_segment[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_summary_timeline_segment[i], LV_OBJ_FLAG_HIDDEN);
    }
    show_timeline_selection(s_summary_timeline_selection);

    if (has_times && started_unix_s) {
        time_t started = (time_t)started_unix_s;
        time_t ended = (time_t)(ended_unix_s ? ended_unix_s : started_unix_s);
        struct tm local_start;
        struct tm local_end;
        localtime_r(&started, &local_start);
        localtime_r(&ended, &local_end);
        char date[32];
        char start_time[12];
        char end_time[12];
        strftime(date, sizeof(date), "%a, %b %d", &local_start);
        strftime(start_time, sizeof(start_time), "%H:%M", &local_start);
        strftime(end_time, sizeof(end_time), "%H:%M", &local_end);
        snprintf(text, sizeof(text), "%s  /  %s - %s", date, start_time, end_time);
        lv_label_set_text(s_summary_period, text);
    } else {
        lv_label_set_text(s_summary_period, "Time unavailable - connect WiFi for NTP");
    }
}

static void refresh_history(void)
{
    sleep_night_record_t nights[7];
    size_t count = 0;
    bool available = sleep_storage_load_recent(nights, 7, &count) == ESP_OK && count;
    if (available) lv_obj_add_flag(s_history_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_history_empty, LV_OBJ_FLAG_HIDDEN);
    uint64_t max_minutes = 1;
    for (size_t i = 0; i < count; ++i) {
        uint64_t minutes = (nights[i].summary.snore_ms + 30000) / 60000;
        if (minutes > max_minutes) max_minutes = minutes;
    }
    for (int i = 0; i < 7; ++i) {
        if ((size_t)i >= count) {
            lv_obj_add_flag(s_history_bar[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_history_value[i], "");
            lv_label_set_text(s_history_date[i], "");
            continue;
        }
        uint64_t minutes = (nights[i].summary.snore_ms + 30000) / 60000;
        int height = 5 + (int)(minutes * 165 / max_minutes);
        if (height > 170) height = 170;
        int x = 35 + i * 96;
        int y = 224 - height;
        lv_obj_set_pos(s_history_bar[i], x, y);
        lv_obj_set_size(s_history_bar[i], 48, height);
        lv_obj_remove_flag(s_history_bar[i], LV_OBJ_FLAG_HIDDEN);
        char text[24];
        snprintf(text, sizeof(text), "%llum", (unsigned long long)minutes);
        lv_label_set_text(s_history_value[i], text);
        lv_obj_set_y(s_history_value[i], y - 22);
        if (nights[i].started_unix_s) {
            time_t timestamp = (time_t)nights[i].started_unix_s;
            struct tm local;
            localtime_r(&timestamp, &local);
            strftime(text, sizeof(text), "%a", &local);
        } else {
            snprintf(text, sizeof(text), "Night %d", i + 1);
        }
        lv_label_set_text(s_history_date[i], text);
    }
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    recorder_status_t status;
    audio_recorder_get_status(&status);
    char text[100];
    lv_label_set_text(s_record_state, status.recording ? "RECORDING" :
                      (status.ready ? (status.sd_mounted ? "READY" : "MIC READY") : "INITIALIZING"));
    snprintf(text, sizeof(text), "%02lu:%02lu", (unsigned long)(status.elapsed_seconds / 60),
             (unsigned long)(status.elapsed_seconds % 60));
    lv_label_set_text(s_record_time, text);
    snprintf(text, sizeof(text), "MIC 1  %.1f dBFS          MIC 2  %.1f dBFS", status.rms_dbfs, status.rms_dbfs_ch2);
    lv_label_set_text(s_record_level, text);
    const float db[2] = {status.rms_dbfs, status.rms_dbfs_ch2};
    for (int i = 0; i < 2; ++i) lv_bar_set_value(s_record_meter[i], LV_CLAMP(0, (int)((db[i] + 72) * 100 / 72), 100), true);
    lv_label_set_text(s_record_file, status.filename[0] ? status.filename :
                      (status.sd_mounted ? "FAT32 microSD ready" : "microSD not mounted"));
    lv_label_set_text(s_record_error, status.error);
    lv_label_set_text(lv_obj_get_child(s_record_button, 0), status.recording ? "STOP & SAVE" : "START RECORDING");
    lv_obj_set_style_bg_color(s_record_button, lv_color_hex(status.recording ? 0xe54b5f : 0x7568ef), 0);
    if (!status.ready || !status.sd_mounted) lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_record_button, LV_STATE_DISABLED);

    sleep_engine_status_t sleep;
    sleep_engine_get_status(&sleep);
    int probability = (int)(sleep.probability * 100 + .5f);
    lv_arc_set_value(s_monitor_ring, probability);
    if (!s_monitor_threshold_dragging) {
        if (sleep.model_available) snprintf(text, sizeof(text), "%d%%", probability);
        else strlcpy(text, "--%", sizeof(text));
        lv_label_set_text(s_monitor_probability, text);
    }

    uint64_t elapsed_seconds = sleep.monitored_ms / 1000;
    snprintf(text, sizeof(text), "%02llu:%02llu:%02llu",
             (unsigned long long)(elapsed_seconds / 3600),
             (unsigned long long)((elapsed_seconds / 60) % 60),
             (unsigned long long)(elapsed_seconds % 60));
    lv_label_set_text(s_monitor_elapsed, text);
    snprintf(text, sizeof(text), "%lu", (unsigned long)sleep.event_count);
    lv_label_set_text(s_monitor_events, text);

    uint32_t accent = 0x8d80ff;
    if (!sleep.model_available || !status.ready) {
        accent = 0xffc46b;
        lv_label_set_text(s_monitor_mode, "SETUP NEEDED");
        lv_label_set_text(s_monitor_state, !sleep.model_available ? "Model unavailable" : "Microphone unavailable");
        lv_label_set_text(s_monitor_detail, "Open Settings and check the local audio hardware.");
    } else if (!status.sd_mounted) {
        accent = 0xffc46b;
        lv_label_set_text(s_monitor_mode, "SD CARD NEEDED");
        lv_label_set_text(s_monitor_state, "Insert microSD");
        lv_label_set_text(s_monitor_detail, "Every night summary is stored on microSD for History.");
    } else if (sleep.event_active) {
        accent = 0xff6b8a;
        lv_label_set_text(s_monitor_mode, "SNORE EVENT");
        lv_label_set_text(s_monitor_state, "Snoring detected");
        lv_label_set_text(s_monitor_detail, "The event is being classified and timed locally.");
    } else if (sleep.monitoring) {
        accent = 0x63e2c6;
        lv_label_set_text(s_monitor_mode, sleep.recording_audio ? "LIVE + WAV" : "LIVE ANALYSIS");
        lv_label_set_text(s_monitor_state, sleep.recording_audio ? "Listening and recording" : "Listening locally");
        lv_label_set_text(s_monitor_detail, sleep.recording_audio ?
                          "Analysis is active and a WAV copy stays on microSD." :
                          "Analysis is active. No audio file is being stored.");
    } else {
        lv_label_set_text(s_monitor_mode, "READY");
        lv_label_set_text(s_monitor_state, "Ready to monitor");
        lv_label_set_text(s_monitor_detail, "Choose analysis or analysis + recording.");
    }
    lv_obj_set_style_bg_color(s_monitor_status_dot, lv_color_hex(accent), 0);
    lv_obj_set_style_text_color(s_monitor_mode, lv_color_hex(accent), 0);
    lv_obj_set_style_arc_color(s_monitor_ring, lv_color_hex(accent), LV_PART_INDICATOR);
    if (sleep.monitoring) {
        lv_obj_add_flag(s_monitor_start_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_monitor_record_button, 360, 48);
        lv_obj_align(s_monitor_record_button, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_label_set_text(lv_obj_get_child(s_monitor_record_button, 0),
                          sleep.recording_audio ? "END + SAVE" : "END NIGHT");
        lv_obj_set_style_bg_color(s_monitor_record_button, lv_color_hex(0xe0526a), 0);
        lv_obj_remove_state(s_monitor_record_button, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_flag(s_monitor_start_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_monitor_start_button, 170, 48);
        lv_obj_align(s_monitor_start_button, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_size(s_monitor_record_button, 178, 48);
        lv_obj_align(s_monitor_record_button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_label_set_text(lv_obj_get_child(s_monitor_start_button, 0), "START");
        lv_label_set_text(lv_obj_get_child(s_monitor_record_button, 0), "START + RECORD");
        lv_obj_set_style_bg_color(s_monitor_start_button, lv_color_hex(0x2fbf91), 0);
        lv_obj_set_style_bg_color(s_monitor_record_button, lv_color_hex(0xe0526a), 0);
        if (!sleep.model_available || !status.ready || !status.sd_mounted)
            lv_obj_add_state(s_monitor_start_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(s_monitor_start_button, LV_STATE_DISABLED);
        if (!sleep.model_available || !status.ready || !status.sd_mounted)
            lv_obj_add_state(s_monitor_record_button, LV_STATE_DISABLED);
        else
            lv_obj_remove_state(s_monitor_record_button, LV_STATE_DISABLED);
    }

    refresh_summary();
    wifi_manager_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (s_wifi_status_value) {
        lv_label_set_text(s_wifi_status_value, wifi.connected ? "Connected" :
                          (s_settings.wifi_ssid[0] ? "Connecting..." : "Not configured"));
        lv_obj_set_style_text_color(s_wifi_status_value,
                                    lv_color_hex(wifi.connected ? 0x63e2c6 : 0xffc46b), 0);
    }
    if (s_time_status_value) {
        char clock_text[32];
        if (wifi.time_synced || wifi_manager_time_valid()) {
            time_t now = time(NULL);
            struct tm local;
            localtime_r(&now, &local);
            strftime(clock_text, sizeof(clock_text), "%H:%M synchronized", &local);
        } else {
            strlcpy(clock_text, wifi.connected ? "Synchronizing..." : "Waiting for NTP", sizeof(clock_text));
        }
        lv_label_set_text(s_time_status_value, clock_text);
        lv_obj_set_style_text_color(s_time_status_value,
                                    lv_color_hex(wifi_manager_time_valid() ? 0x63e2c6 : 0xffc46b), 0);
    }
}

void recorder_ui_create(void)
{
    app_settings_load(&s_settings);
    time_zones_apply(s_settings.timezone_index);
    create_monitor();
    create_summary();
    create_history();
    create_recorder();
    create_settings();
    open_page(PAGE_MONITOR);
    lv_timer_create(refresh, 100, NULL);
    refresh(NULL);
}
