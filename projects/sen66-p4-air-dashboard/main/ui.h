#pragma once

#include <stdbool.h>
#include "sen66.h"

void air_ui_create(void);
void air_ui_update(const sen66_data_t *data, bool connected, unsigned sample_age_seconds);
void air_ui_set_display_enabled(bool enabled);
bool air_ui_display_enabled(void);
