#include "time_zones.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *s_options =
    "Europe/Berlin (auto DST)\nUTC-12:00\nUTC-11:00\nUTC-10:00\nUTC-09:30\nUTC-09:00\n"
    "UTC-08:00\nUTC-07:00\nUTC-06:00\nUTC-05:00\nUTC-04:00\nUTC-03:30\nUTC-03:00\n"
    "UTC-02:00\nUTC-01:00\nUTC+00:00\nUTC+01:00\nUTC+02:00\nUTC+03:00\nUTC+03:30\n"
    "UTC+04:00\nUTC+04:30\nUTC+05:00\nUTC+05:30\nUTC+05:45\nUTC+06:00\nUTC+06:30\n"
    "UTC+07:00\nUTC+08:00\nUTC+08:45\nUTC+09:00\nUTC+09:30\nUTC+10:00\nUTC+10:30\n"
    "UTC+11:00\nUTC+12:00\nUTC+12:45\nUTC+13:00\nUTC+14:00";

static const int16_t s_offsets_minutes[] = {
    0, -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180,
    -120, -60, 0, 60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 390, 420,
    480, 525, 540, 570, 600, 630, 660, 720, 765, 780, 840,
};

const char *time_zones_dropdown_options(void)
{
    return s_options;
}

size_t time_zones_count(void)
{
    return sizeof(s_offsets_minutes) / sizeof(s_offsets_minutes[0]);
}

const char *time_zones_name(uint8_t index)
{
    static char name[20];
    if (index == 0) return "Europe/Berlin";
    if (index >= time_zones_count()) index = 0;
    int offset = s_offsets_minutes[index];
    snprintf(name, sizeof(name), "UTC%c%02d:%02d", offset < 0 ? '-' : '+',
             abs(offset) / 60, abs(offset) % 60);
    return name;
}

esp_err_t time_zones_apply(uint8_t index)
{
    if (index >= time_zones_count()) return ESP_ERR_INVALID_ARG;
    if (index == 0) {
        if (setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1) != 0) return ESP_FAIL;
    } else {
        int west = -s_offsets_minutes[index];
        char rule[24];
        snprintf(rule, sizeof(rule), "UTC%c%d:%02d", west < 0 ? '-' : '+',
                 abs(west) / 60, abs(west) % 60);
        if (setenv("TZ", rule, 1) != 0) return ESP_FAIL;
    }
    tzset();
    return ESP_OK;
}
