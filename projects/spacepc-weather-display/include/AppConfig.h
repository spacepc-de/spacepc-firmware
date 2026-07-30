// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

struct AppConfig {
  String owmApiKey;
  String weatherLocation = "Berlin";
  String countryCode = "DE";
  uint16_t refreshMinutes = 15;

  bool weatherReady() const {
    return owmApiKey.length() >= 20 && !weatherLocation.isEmpty();
  }
};
