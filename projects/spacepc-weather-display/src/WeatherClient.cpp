// SPDX-License-Identifier: GPL-3.0-or-later
#include "WeatherClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace {
String encode(String value) {
  value.replace(" ", "%20");
  value.replace(",", "%2C");
  return value;
}

bool getJson(const String& url, JsonDocument& document, String& errorMessage) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    errorMessage = "Could not start weather request";
    return false;
  }
  http.setTimeout(12000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    errorMessage = code == HTTP_CODE_UNAUTHORIZED
        ? "OpenWeatherMap rejected the API key"
        : "Weather service returned HTTP " + String(code);
    http.end();
    return false;
  }
  const DeserializationError jsonError = deserializeJson(document, http.getStream());
  http.end();
  if (jsonError) {
    errorMessage = "Invalid weather response";
    return false;
  }
  return true;
}
}

String WeatherClient::endpoint(const AppConfig& config, const char* resource) const {
  String query = encode(config.weatherLocation);
  if (!config.countryCode.isEmpty()) query += "%2C" + encode(config.countryCode);
  return "https://api.openweathermap.org/data/2.5/" + String(resource) +
      "?q=" + query + "&appid=" + config.owmApiKey + "&units=metric&lang=en";
}

bool WeatherClient::fetch(const AppConfig& config, WeatherData& data) {
  data = WeatherData();
  if (!config.weatherReady()) {
    data.error = "OpenWeatherMap is not configured";
    return false;
  }
  if (!fetchCurrent(config, data)) return false;
  fetchForecast(config, data);
  data.available = true;
  return true;
}

bool WeatherClient::fetchCurrent(const AppConfig& config, WeatherData& data) {
  JsonDocument doc;
  if (!getJson(endpoint(config, "weather"), doc, data.error)) return false;
  data.location = doc["name"].as<String>();
  data.description = doc["weather"][0]["description"].as<String>();
  data.weatherId = doc["weather"][0]["id"] | 0;
  data.temperature = doc["main"]["temp"] | 0.0f;
  data.feelsLike = doc["main"]["feels_like"] | 0.0f;
  data.minimum = doc["main"]["temp_min"] | 0.0f;
  data.maximum = doc["main"]["temp_max"] | 0.0f;
  data.humidity = doc["main"]["humidity"] | 0.0f;
  data.pressure = doc["main"]["pressure"] | 0.0f;
  data.windSpeed = doc["wind"]["speed"] | 0.0f;
  data.windDirection = doc["wind"]["deg"] | 0.0f;
  data.visibilityKm = (doc["visibility"] | 0.0f) / 1000.0f;
  data.precipitation = doc["rain"]["1h"] | (doc["snow"]["1h"] | 0.0f);
  data.sunrise = doc["sys"]["sunrise"] | 0;
  data.sunset = doc["sys"]["sunset"] | 0;
  data.observedAt = doc["dt"] | 0;
  data.timezoneOffset = doc["timezone"] | 0;
  return true;
}

bool WeatherClient::fetchForecast(const AppConfig& config, WeatherData& data) {
  JsonDocument doc;
  String forecastError;
  if (!getJson(endpoint(config, "forecast"), doc, forecastError)) {
    Serial.println(forecastError);
    return false;
  }

  for (JsonObject item : doc["list"].as<JsonArray>()) {
    const time_t stamp = item["dt"] | 0;
    if (data.hourlyCount < 12) {
      HourlyPoint& point = data.hourly[data.hourlyCount++];
      point.timestamp = stamp;
      point.temperature = item["main"]["temp"] | 0.0f;
      point.precipitation = item["pop"] | 0.0f;
    }

    time_t localStamp = stamp + data.timezoneOffset;
    struct tm timeInfo;
    gmtime_r(&localStamp, &timeInfo);
    int dayIndex = -1;
    for (int i = 0; i < data.forecastCount; ++i) {
      if (data.forecast[i].dayOfYear == timeInfo.tm_yday) {
        dayIndex = i;
        break;
      }
    }
    if (dayIndex < 0) {
      if (data.forecastCount == 5) continue;
      dayIndex = data.forecastCount++;
      data.forecast[dayIndex].dayOfYear = timeInfo.tm_yday;
      data.forecast[dayIndex].timestamp = stamp;
    }

    ForecastDay& day = data.forecast[dayIndex];
    const float low = item["main"]["temp_min"] | 0.0f;
    const float high = item["main"]["temp_max"] | 0.0f;
    day.minimum = min(day.minimum, low);
    day.maximum = max(day.maximum, high);
    day.precipitation = max(day.precipitation, item["pop"] | 0.0f);
    const uint8_t noonDistance = abs(timeInfo.tm_hour - 12);
    if (day.weatherId == 0 || noonDistance < day.representativeDistance) {
      day.representativeDistance = noonDistance;
      day.timestamp = stamp;
      day.temperature = item["main"]["temp"] | 0.0f;
      day.weatherId = item["weather"][0]["id"] | 0;
      day.description = item["weather"][0]["description"].as<String>();
    }
  }
  return data.forecastCount > 0;
}
