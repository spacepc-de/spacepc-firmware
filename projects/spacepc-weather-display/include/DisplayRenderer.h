// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "WeatherClient.h"

class DisplayRenderer {
 public:
  void begin();
  void showDashboard(const WeatherData& weather, int32_t wifiRssi);
  void showSetup(const String& accessPoint);
  void showError(const String& title, const String& detail, const String& action);
  void powerOff();

 private:
  void drawDashboard(const WeatherData& weather, int32_t wifiRssi);
  void drawMessage(const String& eyebrow, const String& title,
                   const String& detail, const String& action);
  void drawCentered(const String& text, int16_t centerX, int16_t baselineY,
                    uint8_t size, uint16_t color);
  void drawWeatherIcon(int16_t x, int16_t y, int weatherId, float scale, uint16_t color);
  void drawWindCompass(int16_t x, int16_t y, float degrees);
  void drawMoonPhase(int16_t x, int16_t y, time_t timestamp);
  void drawGraph(int16_t x, int16_t y, int16_t width, int16_t height,
                 const WeatherData& weather, bool precipitation);
  void drawWifi(int16_t x, int16_t y, int32_t rssi);
  String formatTime(time_t timestamp, int32_t offset) const;
  String dayName(time_t timestamp, int32_t offset) const;
  String titleCase(String text) const;
};
