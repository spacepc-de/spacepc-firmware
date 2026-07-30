// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

#include "AppConfig.h"

struct ForecastDay {
  time_t timestamp = 0;
  float temperature = 0;
  float minimum = 999;
  float maximum = -999;
  int weatherId = 0;
  String description;
  float precipitation = 0;
  int dayOfYear = -1;
  uint8_t representativeDistance = 24;
};

struct HourlyPoint {
  time_t timestamp = 0;
  float temperature = 0;
  float precipitation = 0;
};

struct WeatherData {
  String location;
  String description;
  int weatherId = 0;
  float temperature = 0;
  float feelsLike = 0;
  float minimum = 0;
  float maximum = 0;
  float humidity = 0;
  float pressure = 0;
  float windSpeed = 0;
  float windDirection = 0;
  float visibilityKm = 0;
  float precipitation = 0;
  time_t sunrise = 0;
  time_t sunset = 0;
  time_t observedAt = 0;
  int32_t timezoneOffset = 0;
  ForecastDay forecast[5];
  uint8_t forecastCount = 0;
  HourlyPoint hourly[12];
  uint8_t hourlyCount = 0;
  String error;
  bool available = false;
};

class WeatherClient {
 public:
  bool fetch(const AppConfig& config, WeatherData& data);

 private:
  bool fetchCurrent(const AppConfig& config, WeatherData& data);
  bool fetchForecast(const AppConfig& config, WeatherData& data);
  String endpoint(const AppConfig& config, const char* resource) const;
};
