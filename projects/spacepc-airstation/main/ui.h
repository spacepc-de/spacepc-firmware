#pragma once

#include <stdbool.h>
#include "sen66.h"

void air_ui_create(void);
void air_ui_update(const sen66_data_t *data, bool connected, unsigned sample_age_seconds);
void air_ui_set_display_enabled(bool enabled);
bool air_ui_display_enabled(void);
bool air_ui_temperature_fahrenheit(void);
float air_ui_temperature_for_display(float celsius);
const char *air_ui_temperature_unit_symbol(void);
