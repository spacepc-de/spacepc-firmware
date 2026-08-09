// SPDX-License-Identifier: GPL-3.0-or-later
#include "DisplayRenderer.h"

#include <GxEPD2_3C.h>
#include <SPI.h>

#include "BoardConfig.h"

namespace {
GxEPD2_3C<GxEPD2_750c_GDEY075Z08, GxEPD2_750c_GDEY075Z08::HEIGHT / 2> display(
    GxEPD2_750c_GDEY075Z08(Board::EPD_CS, Board::EPD_DC, Board::EPD_RST, Board::EPD_BUSY));

void thickLine(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color) {
  display.drawLine(x1, y1, x2, y2, color);
  display.drawLine(x1 + 1, y1, x2 + 1, y2, color);
}
}

void DisplayRenderer::begin() {
  SPI.end();
  SPI.begin(Board::EPD_SCK, Board::EPD_MISO, Board::EPD_MOSI, Board::EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(0);
}

void DisplayRenderer::showDashboard(const WeatherData& weather, int32_t wifiRssi) {
  display.setFullWindow();
  display.firstPage();
  do {
    drawDashboard(weather, wifiRssi);
  } while (display.nextPage());
}

void DisplayRenderer::showSetup(const String& accessPoint) {
  display.setFullWindow();
  display.firstPage();
  do {
    drawMessage("FIRST-TIME SETUP", "Connect this display",
                "Wi-Fi and weather access are not configured yet.",
                "1  On your phone, join Wi-Fi:  " + accessPoint +
                "\n2  Open the sign-in page that appears"
                "\n3  Enter Wi-Fi, location and OpenWeatherMap API key");
  } while (display.nextPage());
}

void DisplayRenderer::showError(
    const String& title, const String& detail, const String& action) {
  display.setFullWindow();
  display.firstPage();
  do {
    drawMessage("ACTION REQUIRED", title, detail, action);
  } while (display.nextPage());
}

void DisplayRenderer::drawMessage(
    const String& eyebrow, const String& title,
    const String& detail, const String& action) {
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, Board::DISPLAY_WIDTH, 18, GxEPD_RED);
  display.setTextColor(GxEPD_RED);
  display.setTextSize(2);
  display.setCursor(48, 62);
  display.print(eyebrow);

  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(4);
  display.setCursor(48, 118);
  display.print(title);
  display.drawLine(48, 142, 752, 142, GxEPD_BLACK);

  display.setTextSize(2);
  display.setCursor(48, 184);
  display.print(detail);

  display.fillRoundRect(48, 222, 704, 190, 14, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setTextSize(2);
  int16_t y = 266;
  int start = 0;
  while (start < action.length()) {
    int end = action.indexOf('\n', start);
    if (end < 0) end = action.length();
    display.setCursor(76, y);
    display.print(action.substring(start, end));
    start = end + 1;
    y += 42;
  }
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);
  display.setCursor(48, 454);
  display.print("SpacePC Weather Display  |  setup is stored locally on this device");
}

void DisplayRenderer::drawDashboard(const WeatherData& weather, int32_t wifiRssi) {
  display.fillScreen(GxEPD_WHITE);

  // Minimal location and freshness line.
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(2);
  display.setCursor(22, 22);
  display.print(weather.location);
  drawWifi(666, 28, wifiRssi);
  display.setTextSize(1);
  display.setCursor(696, 20);
  display.print("UPDATED " + formatTime(weather.observedAt, weather.timezoneOffset));
  display.drawLine(20, 41, 780, 41, GxEPD_BLACK);

  // Current weather.
  display.setTextSize(7);
  display.setCursor(20, 86);
  display.print(String(weather.temperature, 0));
  display.setTextSize(3);
  display.setCursor(115, 70);
  display.print("o");
  display.setCursor(151, 86);
  display.print("C");
  display.setTextSize(2);
  display.setCursor(24, 159);
  display.print(titleCase(weather.description).substring(0, 28));
  display.setTextColor(GxEPD_RED);
  display.setCursor(24, 186);
  display.print("Feels like " + String(weather.feelsLike, 0) + " C");

  drawWeatherIcon(292, 105, weather.weatherId, 2.2f, GxEPD_BLACK);

  // Wind gets a real compass instead of a text-only reading.
  drawWindCompass(472, 111, weather.windDirection);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(2);
  const String windSpeed = String(weather.windSpeed * 3.6f, 0) + " KM/H";
  display.setCursor(472 - windSpeed.length() * 6, 190);
  display.print(windSpeed);

  // Current facts.
  display.drawRoundRect(545, 55, 233, 137, 10, GxEPD_BLACK);
  const char* labels[] = {"HUMIDITY", "PRESSURE", "RAIN", "SUN"};
  const bool sunIsUp =
      weather.observedAt >= weather.sunrise && weather.observedAt < weather.sunset;
  const time_t nextSunEvent = sunIsUp
      ? weather.sunset
      : (weather.observedAt < weather.sunrise
             ? weather.sunrise
             : weather.sunrise + 24 * 60 * 60);
  String facts[] = {
      String(weather.humidity, 0) + "%",
      String(weather.pressure, 0) + " hPa",
      String(weather.precipitation, 1) + " mm",
      formatTime(nextSunEvent, weather.timezoneOffset)};
  for (int i = 0; i < 4; ++i) {
    const int col = i % 2;
    const int row = i / 2;
    const int x = 562 + col * 108;
    const int y = 81 + row * 56;
    display.setTextColor(GxEPD_RED);
    display.setTextSize(1);
    display.setCursor(x, y);
    display.print(i == 3 ? (sunIsUp ? "SUNSET" : "SUNRISE") : labels[i]);
    if (i == 3) {
      display.setTextColor(GxEPD_BLACK);
      display.setTextSize(2);
      display.setCursor(x, y + 20);
      display.print(facts[i]);
    } else {
      display.setTextColor(GxEPD_BLACK);
      display.setTextSize(2);
      display.setCursor(x, y + 20);
      display.print(facts[i]);
    }
  }

  drawMoonPhase(370, 170, weather.observedAt);

  // Daily cards use the true minimum and maximum across every 3-hour slot.
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);
  display.setCursor(22, 216);
  display.print("DAILY FORECAST");
  const int16_t forecastY = 226;
  const int16_t cardW = 148;
  for (int i = 0; i < 5; ++i) {
    const int16_t x = 20 + i * 154;
    display.drawRoundRect(x, forecastY, cardW, 101, 9, GxEPD_BLACK);
    if (i >= weather.forecastCount) continue;
    const ForecastDay& day = weather.forecast[i];
    display.setTextSize(2);
    display.setTextColor(i == 0 ? GxEPD_RED : GxEPD_BLACK);
    display.setCursor(x + 10, forecastY + 18);
    display.print(dayName(day.timestamp, weather.timezoneOffset));
    drawWeatherIcon(x + 34, forecastY + 49, day.weatherId, 0.58f, GxEPD_BLACK);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(x + 65, forecastY + 49);
    display.print(String(day.maximum, 0) + "/" + String(day.minimum, 0));
    display.setTextSize(1);
    display.setCursor(x + 65, forecastY + 71);
    display.print("HIGH / LOW");
    display.setTextColor(GxEPD_RED);
    display.setCursor(x + 10, forecastY + 85);
    display.print(String(day.precipitation * 100, 0) + "% precipitation");
  }

  // Next 36 hours: two restrained, readable graphs.
  drawGraph(20, 351, 478, 108, weather, false);
  drawGraph(518, 351, 260, 108, weather, true);
}

void DisplayRenderer::drawWeatherIcon(
    int16_t x, int16_t y, int weatherId, float scale, uint16_t color) {
  const int s = max(2, (int)(10 * scale));
  const bool thunder = weatherId >= 200 && weatherId < 300;
  const bool rain = weatherId >= 300 && weatherId < 600;
  const bool snow = weatherId >= 600 && weatherId < 700;
  const bool atmosphere = weatherId >= 700 && weatherId < 800;
  const bool clear = weatherId == 800;

  if (clear) {
    display.drawCircle(x, y, s, color);
    display.drawCircle(x, y, s - 1, color);
    for (int i = 0; i < 8; ++i) {
      const float angle = i * PI / 4.0f;
      thickLine(x + cos(angle) * s * 1.35f, y + sin(angle) * s * 1.35f,
                x + cos(angle) * s * 1.85f, y + sin(angle) * s * 1.85f, color);
    }
    return;
  }

  // Six overlapping puffs create a rounded cloud without a flat lower edge.
  display.fillCircle(x - s, y, s * 0.78f, color);
  display.fillCircle(x, y - s * 0.42f, s * 1.08f, color);
  display.fillCircle(x + s, y, s * 0.78f, color);
  display.fillCircle(x - s, y + s * 0.42f, s * 0.66f, color);
  display.fillCircle(x, y + s * 0.52f, s * 0.76f, color);
  display.fillCircle(x + s, y + s * 0.42f, s * 0.66f, color);

  if (atmosphere) {
    for (int i = 0; i < 3; ++i)
      thickLine(x - s * 1.5f, y + s + i * s / 2, x + s * 1.5f, y + s + i * s / 2, color);
  } else if (snow) {
    for (int i = -1; i <= 1; ++i) {
      const int px = x + i * s;
      thickLine(px - 4, y + s + 4, px + 4, y + s + 12, color);
      thickLine(px + 4, y + s + 4, px - 4, y + s + 12, color);
    }
  } else if (rain || thunder) {
    for (int i = -1; i <= 1; ++i)
      thickLine(x + i * s - 4, y + s, x + i * s - 10, y + s * 1.65f, color);
    if (thunder)
      display.fillTriangle(x, y + s, x - s / 2, y + s * 2, x + s / 3, y + s * 1.7f, GxEPD_RED);
  }
}

void DisplayRenderer::drawMoonPhase(int16_t x, int16_t y, time_t timestamp) {
  if (!timestamp) return;
  constexpr double synodicMonth = 29.53058867;
  // 2000-01-06 18:14 UTC was a known new moon.
  double age = fmod(((double)timestamp / 86400.0 - 10962.7597), synodicMonth);
  if (age < 0) age += synodicMonth;
  const double phase = age / synodicMonth;
  const uint8_t phaseIndex = ((uint8_t)round(phase * 8.0)) % 8;
  constexpr int radius = 20;

  for (int dy = -radius; dy <= radius; ++dy) {
    const int edge = sqrt(radius * radius - dy * dy);
    for (int dx = -edge; dx <= edge; ++dx) {
      bool illuminated = false;
      switch (phaseIndex) {
        case 1: illuminated = dx > edge * 0.55f; break;   // waxing crescent
        case 2: illuminated = dx >= 0; break;              // first quarter
        case 3: illuminated = dx > -edge * 0.55f; break;  // waxing gibbous
        case 4: illuminated = true; break;                 // full
        case 5: illuminated = dx < edge * 0.55f; break;   // waning gibbous
        case 6: illuminated = dx <= 0; break;              // last quarter
        case 7: illuminated = dx < -edge * 0.55f; break;  // waning crescent
        default: illuminated = false;                      // new
      }
      display.drawPixel(x + dx, y + dy, illuminated ? GxEPD_WHITE : GxEPD_BLACK);
    }
  }
  display.drawCircle(x, y, radius, GxEPD_BLACK);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);
  display.setCursor(x - 15, y + 33);
  display.print("MOON");
}

void DisplayRenderer::drawWindCompass(int16_t x, int16_t y, float degrees) {
  constexpr int radius = 36;
  display.drawCircle(x, y, radius, GxEPD_BLACK);
  display.drawCircle(x, y, radius - 1, GxEPD_BLACK);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);
  display.setCursor(x - 3, y - radius - 10);
  display.print("N");
  display.setCursor(x - 3, y + radius + 11);
  display.print("S");
  display.setCursor(x - radius - 10, y + 3);
  display.print("W");
  display.setCursor(x + radius + 5, y + 3);
  display.print("E");

  // Meteorological direction: arrow points toward the direction the wind comes from.
  const float angle = (degrees - 90.0f) * PI / 180.0f;
  const int16_t tipX = x + cos(angle) * (radius - 7);
  const int16_t tipY = y + sin(angle) * (radius - 7);
  const int16_t tailX = x - cos(angle) * 20;
  const int16_t tailY = y - sin(angle) * 20;
  const float perpendicular = angle + PI / 2.0f;
  display.fillTriangle(
      tipX, tipY,
      tailX + cos(perpendicular) * 7, tailY + sin(perpendicular) * 7,
      tailX - cos(perpendicular) * 7, tailY - sin(perpendicular) * 7,
      GxEPD_RED);
}

void DisplayRenderer::drawGraph(
    int16_t x, int16_t y, int16_t width, int16_t height,
    const WeatherData& weather, bool precipitation) {
  display.drawRoundRect(x, y, width, height, 8, GxEPD_BLACK);
  display.setTextSize(1);
  display.setTextColor(precipitation ? GxEPD_RED : GxEPD_BLACK);
  display.setCursor(x + 12, y + 17);
  display.print(precipitation ? "PRECIPITATION  NEXT 36H" : "TEMPERATURE  NEXT 36H");
  if (weather.hourlyCount < 2) return;

  const int16_t plotX = x + (precipitation ? 12 : 42);
  const int16_t plotY = y + 29;
  const int16_t plotW = width - (precipitation ? 24 : 54);
  const int16_t plotH = height - 48;
  float minimum = precipitation ? 0.0f : weather.hourly[0].temperature;
  float maximum = precipitation ? 1.0f : weather.hourly[0].temperature;
  for (int i = 1; i < weather.hourlyCount; ++i) {
    const float value = precipitation
        ? weather.hourly[i].precipitation : weather.hourly[i].temperature;
    minimum = min(minimum, value);
    maximum = max(maximum, value);
  }
  if (!precipitation && maximum - minimum < 4.0f) {
    const float midpoint = (maximum + minimum) / 2.0f;
    minimum = midpoint - 2.0f;
    maximum = midpoint + 2.0f;
  }

  display.drawLine(plotX, plotY + plotH, plotX + plotW, plotY + plotH, GxEPD_BLACK);
  if (!precipitation) {
    display.drawLine(plotX, plotY, plotX, plotY + plotH, GxEPD_BLACK);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(1);
    display.setCursor(x + 7, plotY + 5);
    display.print(String(maximum, 0) + " C");
    display.setCursor(x + 7, plotY + plotH);
    display.print(String(minimum, 0) + " C");
    for (int i = 0; i <= 2; ++i) {
      const int16_t gridY = plotY + i * plotH / 2;
      display.drawLine(plotX - 3, gridY, plotX + plotW, gridY,
                       i == 1 ? GxEPD_RED : GxEPD_BLACK);
    }
  }
  int16_t previousX = 0;
  int16_t previousY = 0;
  for (int i = 0; i < weather.hourlyCount; ++i) {
    const float value = precipitation
        ? weather.hourly[i].precipitation : weather.hourly[i].temperature;
    const int16_t pointX =
        plotX + (i * plotW) / max(1, (int)weather.hourlyCount - 1);
    const int16_t pointY =
        plotY + plotH - (int16_t)((value - minimum) / (maximum - minimum) * plotH);
    if (precipitation) {
      display.fillRect(pointX - 2, pointY, 5, plotY + plotH - pointY, GxEPD_RED);
    } else {
      if (i > 0) thickLine(previousX, previousY, pointX, pointY, GxEPD_BLACK);
      display.fillCircle(pointX, pointY, 2, i == weather.hourlyCount - 1
          ? GxEPD_RED : GxEPD_BLACK);
    }
    previousX = pointX;
    previousY = pointY;
  }

  // A vertical dashed separator marks each local calendar-day change.
  for (int i = 1; i < weather.hourlyCount; ++i) {
    time_t previousLocal = weather.hourly[i - 1].timestamp + weather.timezoneOffset;
    time_t currentLocal = weather.hourly[i].timestamp + weather.timezoneOffset;
    struct tm previousInfo;
    struct tm currentInfo;
    gmtime_r(&previousLocal, &previousInfo);
    gmtime_r(&currentLocal, &currentInfo);
    if (previousInfo.tm_yday == currentInfo.tm_yday) continue;
    const int16_t separatorX = plotX +
        (int32_t)(weather.hourly[i].timestamp - weather.hourly[0].timestamp) *
        plotW / max((int32_t)1,
                    (int32_t)(weather.hourly[weather.hourlyCount - 1].timestamp -
                              weather.hourly[0].timestamp));
    for (int16_t separatorY = plotY; separatorY < plotY + plotH; separatorY += 4) {
      display.drawLine(separatorX, separatorY, separatorX, separatorY + 1, GxEPD_RED);
    }
  }

  // Compact two-hour labels sit centered between the plot baseline and frame.
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);
  const time_t axisStart = weather.hourly[0].timestamp;
  const time_t axisEnd = weather.hourly[weather.hourlyCount - 1].timestamp;
  const int32_t span = max((int32_t)1, (int32_t)(axisEnd - axisStart));
  for (time_t tick = axisStart; tick <= axisEnd; tick += 2 * 60 * 60) {
    time_t localTick = tick + weather.timezoneOffset;
    struct tm tickInfo;
    gmtime_r(&localTick, &tickInfo);
    char hour[3];
    strftime(hour, sizeof(hour), "%H", &tickInfo);
    const int16_t tickX = plotX +
        (int32_t)(tick - axisStart) * plotW / span;
    display.drawLine(tickX, plotY + plotH, tickX, plotY + plotH + 2, GxEPD_BLACK);
    display.setCursor(tickX - 6, y + height - 13);
    display.print(hour);
  }
}

void DisplayRenderer::drawWifi(int16_t x, int16_t y, int32_t rssi) {
  const int bars = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -75 ? 2 : 1;
  for (int i = 0; i < 4; ++i) {
    const int barHeight = (i + 1) * 4;
    if (i < bars) {
      display.fillRect(x + i * 5, y - barHeight, 3, barHeight, GxEPD_BLACK);
    } else {
      display.drawRect(x + i * 5, y - barHeight, 3, barHeight, GxEPD_BLACK);
    }
  }
}

String DisplayRenderer::formatTime(time_t timestamp, int32_t offset) const {
  if (!timestamp) return "--:--";
  timestamp += offset;
  struct tm timeInfo;
  gmtime_r(&timestamp, &timeInfo);
  char buffer[6];
  strftime(buffer, sizeof(buffer), "%H:%M", &timeInfo);
  return String(buffer);
}

String DisplayRenderer::dayName(time_t timestamp, int32_t offset) const {
  static const char* days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  timestamp += offset;
  struct tm timeInfo;
  gmtime_r(&timestamp, &timeInfo);
  return String(days[timeInfo.tm_wday]);
}

String DisplayRenderer::titleCase(String text) const {
  if (!text.isEmpty()) text[0] = toupper(text[0]);
  return text;
}

void DisplayRenderer::drawCentered(
    const String& text, int16_t centerX, int16_t baselineY, uint8_t size, uint16_t color) {
  int16_t x1, y1;
  uint16_t width, height;
  display.setTextSize(size);
  display.setTextColor(color);
  display.getTextBounds(text, 0, baselineY, &x1, &y1, &width, &height);
  display.setCursor(centerX - width / 2, baselineY);
  display.print(text);
}

void DisplayRenderer::powerOff() {
  display.hibernate();
}
