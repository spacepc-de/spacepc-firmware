#include "recorder_ui.h"

#include <stdio.h>
#include "audio_recorder.h"
#include "esp_log.h"
#include "lvgl.h"

static lv_obj_t *s_state;
static lv_obj_t *s_time;
static lv_obj_t *s_level;
static lv_obj_t *s_meter;
static lv_obj_t *s_meter_ch2;
static lv_obj_t *s_file;
static lv_obj_t *s_error;
static lv_obj_t *s_button;

static void button_event(lv_event_t *event)
{
    (void)event;
    recorder_status_t status;
    audio_recorder_get_status(&status);
    esp_err_t err = status.recording ? audio_recorder_stop() : audio_recorder_start();
    if (err != ESP_OK) ESP_LOGW("recorder_ui", "Toggle failed: %s", esp_err_to_name(err));
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    recorder_status_t status;
    audio_recorder_get_status(&status);
    char text[96];
    lv_label_set_text(s_state, status.recording ? "RECORDING" : (status.ready ? "READY" : "INITIALIZING"));
    snprintf(text, sizeof(text), "%02lu:%02lu", (unsigned long)(status.elapsed_seconds / 60),
             (unsigned long)(status.elapsed_seconds % 60));
    lv_label_set_text(s_time, text);
    snprintf(text, sizeof(text), "MIC 1  %.1f dBFS          MIC 2  %.1f dBFS", status.rms_dbfs, status.rms_dbfs_ch2);
    lv_label_set_text(s_level, text);
    int meter = (int)((status.rms_dbfs + 72.0f) * 100.0f / 72.0f);
    lv_bar_set_value(s_meter, LV_CLAMP(0, meter, 100), true);
    int meter_ch2 = (int)((status.rms_dbfs_ch2 + 72.0f) * 100.0f / 72.0f);
    lv_bar_set_value(s_meter_ch2, LV_CLAMP(0, meter_ch2, 100), true);
    lv_label_set_text(s_file, status.filename[0] ? status.filename : "16 kHz / 16-bit / stereo WAV");
    lv_label_set_text(s_error, status.error);
    lv_obj_t *label = lv_obj_get_child(s_button, 0);
    lv_label_set_text(label, status.recording ? "STOP & SAVE" : "START RECORDING");
    lv_obj_set_style_bg_color(s_button, lv_color_hex(status.recording ? 0xe54b5f : 0x7267f0), 0);
    if (!status.ready) lv_obj_add_state(s_button, LV_STATE_DISABLED); else lv_obj_remove_state(s_button, LV_STATE_DISABLED);
}

void recorder_ui_create(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x080a13), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xf5f7ff), 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Desk Buddy Sleep AI");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 28);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text(subtitle, "Milestone 0  /  Hardware audio recorder");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x8990a8), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 38, 70);

    s_state = lv_label_create(screen);
    lv_obj_set_style_text_color(s_state, lv_color_hex(0x8c83ff), 0);
    lv_obj_set_style_text_font(s_state, &lv_font_montserrat_18, 0);
    lv_obj_align(s_state, LV_ALIGN_TOP_RIGHT, -38, 36);

    s_time = lv_label_create(screen);
    lv_label_set_text(s_time, "00:00");
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -70);

    s_meter = lv_bar_create(screen);
    lv_obj_set_size(s_meter, 650, 14);
    lv_obj_align(s_meter, LV_ALIGN_CENTER, 0, -3);
    lv_obj_set_style_radius(s_meter, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(s_meter, 12, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_meter, lv_color_hex(0x1b2033), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_meter, lv_color_hex(0x54d6c5), LV_PART_INDICATOR);
    lv_bar_set_range(s_meter, 0, 100);

    s_meter_ch2 = lv_bar_create(screen);
    lv_obj_set_size(s_meter_ch2, 650, 14);
    lv_obj_align(s_meter_ch2, LV_ALIGN_CENTER, 0, 19);
    lv_obj_set_style_radius(s_meter_ch2, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(s_meter_ch2, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_meter_ch2, lv_color_hex(0x1b2033), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_meter_ch2, lv_color_hex(0x8c83ff), LV_PART_INDICATOR);
    lv_bar_set_range(s_meter_ch2, 0, 100);

    s_level = lv_label_create(screen);
    lv_label_set_text(s_level, "MIC 1  -96.0 dBFS          MIC 2  -96.0 dBFS");
    lv_obj_set_style_text_color(s_level, lv_color_hex(0xaab0c5), 0);
    lv_obj_align(s_level, LV_ALIGN_CENTER, 0, 51);

    s_button = lv_button_create(screen);
    lv_obj_set_size(s_button, 270, 66);
    lv_obj_align(s_button, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_radius(s_button, 22, 0);
    lv_obj_add_event_cb(s_button, button_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *button_label = lv_label_create(s_button);
    lv_label_set_text(button_label, "START RECORDING");
    lv_obj_set_style_text_font(button_label, &lv_font_montserrat_18, 0);
    lv_obj_center(button_label);

    s_file = lv_label_create(screen);
    lv_obj_set_style_text_color(s_file, lv_color_hex(0x777f99), 0);
    lv_obj_align(s_file, LV_ALIGN_BOTTOM_LEFT, 38, -18);
    s_error = lv_label_create(screen);
    lv_obj_set_style_text_color(s_error, lv_color_hex(0xff7185), 0);
    lv_obj_align(s_error, LV_ALIGN_BOTTOM_RIGHT, -38, -18);

    lv_timer_create(refresh, 100, NULL);
    refresh(NULL);
}
