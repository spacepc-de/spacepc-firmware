#include "recorder_ui.h"

#include <stdio.h>
#include "app_settings.h"
#include "audio_recorder.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "lvgl.h"
#include "snore_classifier.h"
#include "sleep_engine.h"

enum { PAGE_MONITOR, PAGE_SUMMARY, PAGE_HISTORY, PAGE_RECORDER, PAGE_SETTINGS, PAGE_COUNT };

static lv_obj_t *s_pages[PAGE_COUNT];
static lv_obj_t *s_record_state;
static lv_obj_t *s_record_time;
static lv_obj_t *s_record_level;
static lv_obj_t *s_record_meter[2];
static lv_obj_t *s_record_file;
static lv_obj_t *s_record_error;
static lv_obj_t *s_record_button;
static lv_obj_t *s_threshold_value;
static lv_obj_t *s_threshold_slider;
static lv_obj_t *s_brightness_value;
static lv_obj_t *s_schedule_value;
static lv_obj_t *s_gain_value;
static lv_obj_t *s_model_value;
static lv_obj_t *s_record_night_value;
static lv_obj_t *s_profile_button[2];
static lv_obj_t *s_monitor_ring;
static lv_obj_t *s_monitor_probability;
static lv_obj_t *s_monitor_state;
static lv_obj_t *s_monitor_model;
static lv_obj_t *s_monitor_mode;
static lv_obj_t *s_monitor_detail;
static lv_obj_t *s_monitor_status_dot;
static lv_obj_t *s_monitor_elapsed;
static lv_obj_t *s_monitor_events;
static lv_obj_t *s_monitor_start_button;
static lv_obj_t *s_monitor_record_button;
static app_settings_t s_settings;
static lv_obj_t *s_wake_overlay;
static bool s_display_off;

static void open_page(int page);

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
    lv_obj_t *brand = label(screen, "DESK BUDDY", &lv_font_montserrat_18, 0x9f96ff);
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
    static const char *names[] = {"Monitor", "Summary", "History", "Recorder", "Settings"};
    for (int i = 0; i < PAGE_COUNT; ++i) {
        lv_obj_t *button = lv_button_create(screen);
        lv_obj_set_size(button, i == PAGE_SETTINGS ? 102 : 92, 38);
        lv_obj_set_pos(button, 220 + i * 108, 10);
        lv_obj_set_style_radius(button, 14, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(i == active ? 0x7568ef : 0x111625), 0);
        lv_obj_add_event_cb(button, nav_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
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
    lv_obj_t *sub = label(screen, subtitle, &lv_font_montserrat_14, 0x838ba3);
    lv_obj_set_pos(sub, 30, 107);
    return screen;
}

static void create_monitor(void)
{
    lv_obj_t *screen = make_screen(PAGE_MONITOR, "Tonight", "Private sleep acoustics, processed entirely on Desk Buddy");

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
    s_monitor_probability = label(hero, "--%", &lv_font_montserrat_48, 0xf8f9ff);
    lv_obj_align(s_monitor_probability, LV_ALIGN_TOP_MID, 0, 79);
    lv_obj_t *caption = label(hero, "SNORE CONFIDENCE", &lv_font_montserrat_14, 0x9aa2ba);
    lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 140);
    s_monitor_model = label(hero, "MODEL NOT INSTALLED", &lv_font_montserrat_18, 0xffc46b);
    lv_obj_align(s_monitor_model, LV_ALIGN_TOP_MID, 0, 190);

    lv_obj_t *divider = lv_obj_create(hero);
    lv_obj_set_size(divider, 278, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x333a55), 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);
    s_monitor_elapsed = label(hero, "00:00", &lv_font_montserrat_18, 0xf4f6ff);
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
    s_monitor_state = label(status, "Your night is private", &lv_font_montserrat_24, 0xf4f6ff);
    lv_obj_set_pos(s_monitor_state, 2, 31);
    s_monitor_detail = label(status, "Choose live analysis or keep a local WAV copy.", &lv_font_montserrat_14, 0x8e97af);
    lv_obj_set_pos(s_monitor_detail, 2, 70);
    lv_obj_t *privacy = label(status, "ON-DEVICE AI   /   NO CLOUD   /   LOCAL STORAGE", &lv_font_montserrat_14, 0x63e2c6);
    lv_obj_align(privacy, LV_ALIGN_BOTTOM_LEFT, 2, 1);

    lv_obj_t *actions = card(screen, 376, 331, 396, 118);
    lv_obj_set_style_bg_color(actions, lv_color_hex(0x15152a), 0);
    lv_obj_set_style_border_color(actions, lv_color_hex(0x433d75), 0);
    lv_obj_t *action_title = label(actions, "START A NIGHT", &lv_font_montserrat_14, 0x9aa2ba);
    lv_obj_set_pos(action_title, 2, -2);

    s_monitor_start_button = lv_button_create(actions);
    lv_obj_set_size(s_monitor_start_button, 170, 48);
    lv_obj_align(s_monitor_start_button, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_radius(s_monitor_start_button, 15, 0);
    lv_obj_set_style_bg_color(s_monitor_start_button, lv_color_hex(0x2a3048), 0);
    lv_obj_set_style_shadow_width(s_monitor_start_button, 0, 0);
    lv_obj_add_event_cb(s_monitor_start_button, monitor_button_event, LV_EVENT_CLICKED, (void *)0);
    lv_obj_t *start_text = label(s_monitor_start_button, "START", &lv_font_montserrat_14, 0xffffff);
    lv_obj_center(start_text);

    s_monitor_record_button = lv_button_create(actions);
    lv_obj_set_size(s_monitor_record_button, 178, 48);
    lv_obj_align(s_monitor_record_button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(s_monitor_record_button, 15, 0);
    lv_obj_set_style_bg_color(s_monitor_record_button, lv_color_hex(0x7568ef), 0);
    lv_obj_set_style_shadow_width(s_monitor_record_button, 0, 0);
    lv_obj_add_event_cb(s_monitor_record_button, monitor_button_event, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *record_text = label(s_monitor_record_button, "START + RECORD", &lv_font_montserrat_14, 0xffffff);
    lv_obj_center(record_text);
    s_pages[PAGE_MONITOR] = screen;
}

static void create_summary(void)
{
    lv_obj_t *screen = make_screen(PAGE_SUMMARY, "Good morning", "Last completed monitoring session");
    lv_obj_t *score = card(screen, 28, 143, 232, 306);
    lv_obj_t *small = label(score, "ACOUSTIC SCORE", &lv_font_montserrat_14, 0x8c94aa);
    lv_obj_align(small, LV_ALIGN_TOP_MID, 0, 3);
    lv_obj_t *number = label(score, "--", &lv_font_montserrat_48, 0x63e2c6);
    lv_obj_align(number, LV_ALIGN_CENTER, 0, -20);
    lv_obj_t *note = label(score, "Available after a\ncomplete model-based session", &lv_font_montserrat_14, 0x8c94aa);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -25);
    const char *values[] = {"Monitored", "Snoring detected", "Events", "Longest phase"};
    const char *empty[] = {"-- h -- min", "-- min", "--", "-- min"};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *item = card(screen, 280 + (i % 2) * 246, 143 + (i / 2) * 157, 226, 139);
        lv_obj_t *a = label(item, values[i], &lv_font_montserrat_14, 0x8c94aa);
        lv_obj_set_pos(a, 1, 2);
        lv_obj_t *b = label(item, empty[i], &lv_font_montserrat_24, 0xf5f7ff);
        lv_obj_set_pos(b, 1, 48);
    }
    s_pages[PAGE_SUMMARY] = screen;
}

static void create_history(void)
{
    lv_obj_t *screen = make_screen(PAGE_HISTORY, "Seven-night trend", "Objective acoustic statistics - no health interpretation");
    lv_obj_t *chart_card = card(screen, 28, 143, 744, 306);
    lv_obj_t *empty = label(chart_card, "NO COMPLETED NIGHTS YET", &lv_font_montserrat_24, 0x9b94ff);
    lv_obj_align(empty, LV_ALIGN_CENTER, 0, -22);
    lv_obj_t *hint = label(chart_card, "Night summaries will appear here as a timeline and weekly comparison.", &lv_font_montserrat_14, 0x858ea5);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 24);
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

static void slider_event(lv_event_t *event)
{
    int key = (int)(intptr_t)lv_event_get_user_data(event);
    lv_obj_t *slider = lv_event_get_target(event);
    int value = lv_slider_get_value(slider);
    char text[40];
    if (key == 0) {
        s_settings.night_brightness = value;
        snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(s_brightness_value, text);
    } else if (key == 1) {
        s_settings.start_probability = value;
        s_settings.end_probability = LV_MAX(5, value * 60 / 100);
        snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(s_threshold_value, text);
    } else {
        value = 24 + ((value - 24 + 1) / 3) * 3;
        if (value > 36) value = 36;
        s_settings.microphone_gain_db = value;
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
        snprintf(text, sizeof(text), "%d dB", value);
        lv_label_set_text(s_gain_value, text);
        esp_err_t err = audio_recorder_set_gain(value);
        if (err != ESP_OK) ESP_LOGW("sleep_ui", "Microphone gain failed: %s", esp_err_to_name(err));
    }
    app_settings_save(&s_settings);
}

static void schedule_switch_event(lv_event_t *event)
{
    s_settings.schedule_enabled = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
    lv_label_set_text(s_schedule_value, s_settings.schedule_enabled ? "23:00 - 07:00 enabled" : "Disabled");
    app_settings_save(&s_settings);
}

static void record_night_switch_event(lv_event_t *event)
{
    s_settings.record_during_monitoring = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
    lv_label_set_text(s_record_night_value, s_settings.record_during_monitoring ? "Analysis + WAV" : "Analysis only");
    app_settings_save(&s_settings);
}

static void update_profile_controls(void)
{
    bool conservative = s_settings.model_profile == APP_MODEL_CONSERVATIVE;
    lv_label_set_text(s_model_value, conservative ? "Balanced" : "Sensitive");
    for (int i = 0; i < 2; ++i) {
        bool selected = (i == APP_MODEL_CONSERVATIVE) == conservative;
        lv_obj_set_style_bg_color(s_profile_button[i], lv_color_hex(selected ? 0x7568ef : 0x252d42), 0);
    }
    char text[24];
    snprintf(text, sizeof(text), "%u%%", s_settings.start_probability);
    lv_label_set_text(s_threshold_value, text);
    lv_slider_set_value(s_threshold_slider, s_settings.start_probability, LV_ANIM_OFF);
}

static void profile_event(lv_event_t *event)
{
    s_settings.model_profile = (uint8_t)(intptr_t)lv_event_get_user_data(event);
    if (s_settings.model_profile == APP_MODEL_CONSERVATIVE) {
        s_settings.start_probability = 50;
        s_settings.end_probability = 25;
    } else {
        s_settings.start_probability = 35;
        s_settings.end_probability = 18;
    }
    update_profile_controls();
    app_settings_save(&s_settings);
}

static void create_settings(void)
{
    lv_obj_t *screen = make_screen(PAGE_SETTINGS, "Settings", "Stored locally and preserved after restart");
    const int xs[] = {28, 280, 532};
    const int card_width = 240;
    const int control_width = 204;
    lv_obj_t *schedule = card(screen, xs[0], 143, card_width, 144);
    lv_obj_t *st = label(schedule, "SLEEP SCHEDULE", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(st, 0, 0);
    s_schedule_value = label(schedule, s_settings.schedule_enabled ? "23:00 - 07:00 enabled" : "Disabled", &lv_font_montserrat_18, 0xf5f7ff);
    lv_obj_set_pos(s_schedule_value, 0, 34);
    lv_obj_t *toggle = lv_switch_create(schedule);
    lv_obj_align(toggle, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    if (s_settings.schedule_enabled) lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_add_event_cb(toggle, schedule_switch_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *brightness = card(screen, xs[1], 143, card_width, 144);
    lv_obj_t *bt = label(brightness, "NIGHT BRIGHTNESS", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(bt, 0, 0);
    char value[24];
    snprintf(value, sizeof(value), "%u%%", s_settings.night_brightness);
    s_brightness_value = label(brightness, value, &lv_font_montserrat_18, 0xf5f7ff);
    lv_obj_align(s_brightness_value, LV_ALIGN_TOP_RIGHT, -1, 0);
    lv_obj_t *bright_slider = lv_slider_create(brightness);
    lv_obj_set_size(bright_slider, control_width, 18);
    lv_obj_align(bright_slider, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_slider_set_range(bright_slider, 0, 30);
    lv_slider_set_value(bright_slider, s_settings.night_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(bright_slider, slider_event, LV_EVENT_VALUE_CHANGED, (void *)0);

    lv_obj_t *profile = card(screen, xs[2], 143, card_width, 144);
    lv_obj_t *pt = label(profile, "SNORING SENSITIVITY", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(pt, 0, 0);
    s_model_value = label(profile, "", &lv_font_montserrat_18, 0x63e2c6);
    lv_obj_align(s_model_value, LV_ALIGN_TOP_RIGHT, 0, 0);
    const char *profile_names[] = {"BALANCED", "SENSITIVE"};
    for (int i = 0; i < 2; ++i) {
        s_profile_button[i] = lv_button_create(profile);
        lv_obj_set_size(s_profile_button[i], 98, 38);
        lv_obj_align(s_profile_button[i], LV_ALIGN_BOTTOM_LEFT, i * 106, 0);
        lv_obj_set_style_radius(s_profile_button[i], 12, 0);
        lv_obj_set_style_shadow_width(s_profile_button[i], 0, 0);
        lv_obj_add_event_cb(s_profile_button[i], profile_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *profile_text = label(s_profile_button[i], profile_names[i], &lv_font_montserrat_14, 0xffffff);
        lv_obj_center(profile_text);
    }

    lv_obj_t *classifier = card(screen, xs[0], 305, card_width, 144);
    lv_obj_t *ct = label(classifier, "EVENT START THRESHOLD", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(ct, 0, 0);
    snprintf(value, sizeof(value), "%u%%", s_settings.start_probability);
    s_threshold_value = label(classifier, value, &lv_font_montserrat_18, 0xf5f7ff);
    lv_obj_align(s_threshold_value, LV_ALIGN_TOP_RIGHT, -1, 0);
    s_threshold_slider = lv_slider_create(classifier);
    lv_obj_set_size(s_threshold_slider, control_width, 18);
    lv_obj_align(s_threshold_slider, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_slider_set_range(s_threshold_slider, 5, 90);
    lv_slider_set_value(s_threshold_slider, s_settings.start_probability, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_threshold_slider, slider_event, LV_EVENT_VALUE_CHANGED, (void *)1);

    lv_obj_t *record = card(screen, xs[1], 305, card_width, 144);
    lv_obj_t *rt = label(record, "RECORD DURING SLEEP", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(rt, 0, 0);
    s_record_night_value = label(record, s_settings.record_during_monitoring ? "Analysis + WAV" : "Analysis only",
                                 &lv_font_montserrat_18, 0xf5f7ff);
    lv_obj_set_pos(s_record_night_value, 0, 34);
    lv_obj_t *record_toggle = lv_switch_create(record);
    lv_obj_align(record_toggle, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    if (s_settings.record_during_monitoring) lv_obj_add_state(record_toggle, LV_STATE_CHECKED);
    lv_obj_add_event_cb(record_toggle, record_night_switch_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *model = card(screen, xs[2], 305, card_width, 144);
    lv_obj_t *mt = label(model, "MICROPHONE GAIN", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(mt, 0, 0);
    snprintf(value, sizeof(value), "%u dB", s_settings.microphone_gain_db);
    s_gain_value = label(model, value, &lv_font_montserrat_18, 0x63e2c6);
    lv_obj_align(s_gain_value, LV_ALIGN_TOP_RIGHT, -1, 0);
    lv_obj_t *gain = lv_slider_create(model);
    lv_obj_set_size(gain, control_width, 18);
    lv_obj_align(gain, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_slider_set_range(gain, 24, 36);
    lv_slider_set_value(gain, s_settings.microphone_gain_db, LV_ANIM_OFF);
    lv_obj_add_event_cb(gain, slider_event, LV_EVENT_VALUE_CHANGED, (void *)2);
    update_profile_controls();
    s_pages[PAGE_SETTINGS] = screen;
}

static void open_page(int page)
{
    if (page < 0 || page >= PAGE_COUNT || !s_pages[page]) return;
    lv_screen_load_anim(s_pages[page], LV_SCR_LOAD_ANIM_FADE_IN, 180, 0, false);
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
    if (sleep.model_available) snprintf(text, sizeof(text), "%d%%", probability);
    else strlcpy(text, "--%", sizeof(text));
    lv_label_set_text(s_monitor_probability, text);

    uint64_t elapsed_seconds = sleep.monitored_ms / 1000;
    snprintf(text, sizeof(text), "%02llu:%02llu", (unsigned long long)(elapsed_seconds / 3600),
             (unsigned long long)((elapsed_seconds / 60) % 60));
    lv_label_set_text(s_monitor_elapsed, text);
    snprintf(text, sizeof(text), "%lu", (unsigned long)sleep.event_count);
    lv_label_set_text(s_monitor_events, text);

    uint32_t accent = 0x8d80ff;
    if (!sleep.model_available || !status.ready) {
        accent = 0xffc46b;
        lv_label_set_text(s_monitor_mode, "SETUP NEEDED");
        lv_label_set_text(s_monitor_state, !sleep.model_available ? "Model unavailable" : "Microphone unavailable");
        lv_label_set_text(s_monitor_detail, "Open Settings and check the local audio hardware.");
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
        lv_label_set_text(s_monitor_state, "Ready when you are");
        lv_label_set_text(s_monitor_detail, status.sd_mounted ?
                          "Choose live analysis or keep a local WAV copy." :
                          "Live analysis is ready. Insert microSD to record.");
    }
    lv_obj_set_style_bg_color(s_monitor_status_dot, lv_color_hex(accent), 0);
    lv_obj_set_style_text_color(s_monitor_mode, lv_color_hex(accent), 0);
    lv_obj_set_style_arc_color(s_monitor_ring, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_label_set_text(s_monitor_model, sleep.model_available ?
                      "AUDIOSET SNORING" :
                      "MODEL NOT INSTALLED");
    lv_obj_set_style_text_color(s_monitor_model, lv_color_hex(sleep.model_available ? 0x63e2c6 : 0xffc46b), 0);

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
        lv_obj_set_style_bg_color(s_monitor_record_button, lv_color_hex(0x7568ef), 0);
        if (!sleep.model_available || !status.ready) lv_obj_add_state(s_monitor_start_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(s_monitor_start_button, LV_STATE_DISABLED);
        if (!sleep.model_available || !status.ready || !status.sd_mounted)
            lv_obj_add_state(s_monitor_record_button, LV_STATE_DISABLED);
        else
            lv_obj_remove_state(s_monitor_record_button, LV_STATE_DISABLED);
    }
}

void recorder_ui_create(void)
{
    app_settings_load(&s_settings);
    create_monitor();
    create_summary();
    create_history();
    create_recorder();
    create_settings();
    open_page(PAGE_MONITOR);
    lv_timer_create(refresh, 100, NULL);
    refresh(NULL);
}
