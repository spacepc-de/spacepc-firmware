#include "recorder_ui.h"

#include <stdio.h>
#include "app_settings.h"
#include "audio_recorder.h"
#include "esp_log.h"
#include "lvgl.h"
#include "snore_classifier.h"

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
static lv_obj_t *s_brightness_value;
static lv_obj_t *s_schedule_value;
static app_settings_t s_settings;

static void open_page(int page);

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
    lv_obj_t *screen = make_screen(PAGE_MONITOR, "Sleep monitoring", "Local audio analysis - no cloud recording");
    lv_obj_t *hero = card(screen, 28, 143, 360, 306);
    lv_obj_t *ring = lv_arc_create(hero);
    lv_obj_set_size(ring, 205, 205);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 4);
    lv_arc_set_rotation(ring, 135);
    lv_arc_set_bg_angles(ring, 0, 270);
    lv_arc_set_value(ring, 0);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(ring, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x283149), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x63e2c6), LV_PART_INDICATOR);
    lv_obj_t *prob = label(hero, "--%", &lv_font_montserrat_48, 0xf8f9ff);
    lv_obj_align(prob, LV_ALIGN_TOP_MID, 0, 77);
    lv_obj_t *caption = label(hero, "SNORE PROBABILITY", &lv_font_montserrat_14, 0x8992aa);
    lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 139);
    lv_obj_t *model = label(hero, "MODEL NOT INSTALLED", &lv_font_montserrat_18, 0xffc46b);
    lv_obj_align(model, LV_ALIGN_BOTTOM_MID, 0, -28);

    lv_obj_t *status = card(screen, 410, 143, 362, 144);
    lv_obj_t *dot = lv_obj_create(status);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xffc46b), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_pos(dot, 4, 5);
    lv_obj_t *waiting = label(status, "Ready for a model", &lv_font_montserrat_24, 0xf4f6ff);
    lv_obj_set_pos(waiting, 30, -2);
    lv_obj_t *explain = label(status, "Recorder works. Classification remains disabled\nuntil a licensed model is installed.", &lv_font_montserrat_14, 0x8e97af);
    lv_obj_set_pos(explain, 4, 48);

    lv_obj_t *privacy = card(screen, 410, 305, 362, 144);
    lv_obj_t *p1 = label(privacy, "PRIVATE BY DESIGN", &lv_font_montserrat_14, 0x63e2c6);
    lv_obj_set_pos(p1, 2, 0);
    lv_obj_t *p2 = label(privacy, "Audio stays on this device", &lv_font_montserrat_24, 0xf4f6ff);
    lv_obj_set_pos(p2, 2, 27);
    lv_obj_t *p3 = label(privacy, "Not a medical diagnosis", &lv_font_montserrat_14, 0x8e97af);
    lv_obj_set_pos(p3, 2, 72);
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
    lv_obj_t *screen = make_screen(PAGE_RECORDER, "Dataset recorder", "16 kHz / 16-bit / stereo WAV - collect real hardware samples");
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
    } else {
        s_settings.start_probability = value;
        snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(s_threshold_value, text);
    }
    app_settings_save(&s_settings);
}

static void switch_event(lv_event_t *event)
{
    s_settings.schedule_enabled = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
    lv_label_set_text(s_schedule_value, s_settings.schedule_enabled ? "23:00 - 07:00 enabled" : "Disabled");
    app_settings_save(&s_settings);
}

static void create_settings(void)
{
    lv_obj_t *screen = make_screen(PAGE_SETTINGS, "Settings", "Stored locally and preserved after restart");
    lv_obj_t *schedule = card(screen, 28, 143, 360, 144);
    lv_obj_t *st = label(schedule, "AUTOMATIC SCHEDULE", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(st, 0, 0);
    s_schedule_value = label(schedule, s_settings.schedule_enabled ? "23:00 - 07:00 enabled" : "Disabled", &lv_font_montserrat_18, 0xf5f7ff);
    lv_obj_set_pos(s_schedule_value, 0, 39);
    lv_obj_t *toggle = lv_switch_create(schedule);
    lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -3, 9);
    if (s_settings.schedule_enabled) lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_add_event_cb(toggle, switch_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *brightness = card(screen, 410, 143, 362, 144);
    lv_obj_t *bt = label(brightness, "NIGHT BRIGHTNESS", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(bt, 0, 0);
    char value[24];
    snprintf(value, sizeof(value), "%u%%", s_settings.night_brightness);
    s_brightness_value = label(brightness, value, &lv_font_montserrat_18, 0xf5f7ff);
    lv_obj_align(s_brightness_value, LV_ALIGN_TOP_RIGHT, -1, 0);
    lv_obj_t *bright_slider = lv_slider_create(brightness);
    lv_obj_set_size(bright_slider, 312, 18);
    lv_obj_align(bright_slider, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_slider_set_range(bright_slider, 0, 30);
    lv_slider_set_value(bright_slider, s_settings.night_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(bright_slider, slider_event, LV_EVENT_VALUE_CHANGED, (void *)0);

    lv_obj_t *classifier = card(screen, 28, 305, 360, 144);
    lv_obj_t *ct = label(classifier, "EVENT START THRESHOLD", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(ct, 0, 0);
    snprintf(value, sizeof(value), "%u%%", s_settings.start_probability);
    s_threshold_value = label(classifier, value, &lv_font_montserrat_18, 0xf5f7ff);
    lv_obj_align(s_threshold_value, LV_ALIGN_TOP_RIGHT, -1, 0);
    lv_obj_t *threshold = lv_slider_create(classifier);
    lv_obj_set_size(threshold, 312, 18);
    lv_obj_align(threshold, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_slider_set_range(threshold, 50, 98);
    lv_slider_set_value(threshold, s_settings.start_probability, LV_ANIM_OFF);
    lv_obj_add_event_cb(threshold, slider_event, LV_EVENT_VALUE_CHANGED, (void *)1);

    lv_obj_t *model = card(screen, 410, 305, 362, 144);
    lv_obj_t *mt = label(model, "CLASSIFIER", &lv_font_montserrat_14, 0x8d95aa);
    lv_obj_set_pos(mt, 0, 0);
    lv_obj_t *mv = label(model, "No model installed", &lv_font_montserrat_18, 0xffc46b);
    lv_obj_set_pos(mv, 0, 34);
    lv_obj_t *md = label(model, "Recorder and analytics are ready.\nInference activates after model import.", &lv_font_montserrat_14, 0x858ea5);
    lv_obj_set_pos(md, 0, 68);
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
    lv_label_set_text(s_record_state, status.recording ? "RECORDING" : (status.ready ? "READY" : "INITIALIZING"));
    snprintf(text, sizeof(text), "%02lu:%02lu", (unsigned long)(status.elapsed_seconds / 60),
             (unsigned long)(status.elapsed_seconds % 60));
    lv_label_set_text(s_record_time, text);
    snprintf(text, sizeof(text), "MIC 1  %.1f dBFS          MIC 2  %.1f dBFS", status.rms_dbfs, status.rms_dbfs_ch2);
    lv_label_set_text(s_record_level, text);
    const float db[2] = {status.rms_dbfs, status.rms_dbfs_ch2};
    for (int i = 0; i < 2; ++i) lv_bar_set_value(s_record_meter[i], LV_CLAMP(0, (int)((db[i] + 72) * 100 / 72), 100), true);
    lv_label_set_text(s_record_file, status.filename[0] ? status.filename : "FAT32 microSD ready");
    lv_label_set_text(s_record_error, status.error);
    lv_label_set_text(lv_obj_get_child(s_record_button, 0), status.recording ? "STOP & SAVE" : "START RECORDING");
    lv_obj_set_style_bg_color(s_record_button, lv_color_hex(status.recording ? 0xe54b5f : 0x7568ef), 0);
    if (!status.ready) lv_obj_add_state(s_record_button, LV_STATE_DISABLED); else lv_obj_remove_state(s_record_button, LV_STATE_DISABLED);
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
