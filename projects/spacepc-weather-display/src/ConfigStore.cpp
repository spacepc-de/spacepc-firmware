// SPDX-License-Identifier: GPL-3.0-or-later
#include "ConfigStore.h"

#include <Preferences.h>

namespace {
// Preferences namespaces may contain at most 15 characters.
constexpr const char* NAMESPACE = "spc-weather";
}

bool ConfigStore::load(AppConfig& config) {
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, true)) return false;
  config.owmApiKey = prefs.getString("owm_key", "");
  config.weatherLocation = prefs.getString("owm_city", config.weatherLocation);
  config.countryCode = prefs.getString("owm_country", config.countryCode);
  config.refreshMinutes = prefs.getUShort("refresh", config.refreshMinutes);
  prefs.end();
  return true;
}

bool ConfigStore::save(const AppConfig& config) {
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, false)) return false;
  prefs.putString("owm_key", config.owmApiKey);
  prefs.putString("owm_city", config.weatherLocation);
  prefs.putString("owm_country", config.countryCode);
  prefs.putUShort("refresh", constrain(config.refreshMinutes, 5, 1440));
  prefs.end();
  return true;
}

void ConfigStore::clear() {
  Preferences prefs;
  if (prefs.begin(NAMESPACE, false)) {
    prefs.clear();
    prefs.end();
  }
}
