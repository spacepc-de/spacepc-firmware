#include <Arduino.h>
#include <ArduinoJson.h>
#include <GxEPD2_3C.h>
#include <SpacePCPortal.h>

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

namespace {
constexpr uint16_t screenWidth = 800;
constexpr uint16_t screenHeight = 480;
constexpr uint8_t maximumWidgets = 6;
constexpr uint8_t maximumGraphPoints = 48;
constexpr uint32_t minimumRefreshMs = 300000;
constexpr int16_t dashboardHeaderHeight = 44;
constexpr int16_t cardGap = 10;
constexpr int16_t cardRadius = 9;

GxEPD2_3C<
  GxEPD2_750c_GDEY075Z08,
  GxEPD2_750c_GDEY075Z08::HEIGHT / 2
> display(
  GxEPD2_750c_GDEY075Z08(27, 14, 12, 13)
);
SpacePCPortal portal;
String pendingPayload;
String currentPayload;
uint32_t lastRefresh = 0;
bool displayReady = false;

const SpacePCPortalConfig projectConfig = {
  "spacepc-homeassistant-display",
  "Home Assistant e-paper display",
  "Good Display GDEY075Z08 + ESP32-L",
  "0.1.1",
  "2026-07-29",
  nullptr,
  "Display"
};

const char displayCapabilities[] =
  "{\"width\":800,\"height\":480,\"colors\":[\"black\",\"white\",\"red\"],"
  "\"max_widgets\":6,\"max_graph_points\":48,\"partial_refresh\":false,"
  "\"minimum_refresh_seconds\":300,"
  "\"widget_types\":[\"value\",\"status\",\"graph\"]}";

String limitedText(const char *value, size_t maximumLength) {
  String result = value ? value : "";
  if (result.length() > maximumLength) {
    result = result.substring(0, maximumLength - 1) + "…";
  }
  return result;
}

void drawText(
  const String &text,
  int16_t x,
  int16_t y,
  const GFXfont *font,
  uint16_t color = GxEPD_BLACK
) {
  display.setFont(font);
  display.setTextColor(color);
  display.setCursor(x, y);
  display.print(text);
}

int16_t textWidth(const String &text, const GFXfont *font) {
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
  display.setFont(font);
  display.getTextBounds(text, 0, 0, &x, &y, &width, &height);
  return static_cast<int16_t>(width);
}

void drawRightAligned(
  const String &text,
  int16_t right,
  int16_t baseline,
  const GFXfont *font,
  uint16_t color = GxEPD_BLACK
) {
  drawText(text, right - textWidth(text, font), baseline, font, color);
}

void drawCentered(
  const String &text,
  int16_t center,
  int16_t baseline,
  const GFXfont *font,
  uint16_t color = GxEPD_BLACK
) {
  drawText(text, center - textWidth(text, font) / 2, baseline, font, color);
}

void renderOfflineBadge(int16_t x, int16_t y, int16_t width, int16_t height) {
  constexpr char offlineLabel[] = "OFFLINE";
  const int16_t labelWidth = textWidth(offlineLabel, &FreeSansBold9pt7b);
  const int16_t badgeWidth = labelWidth + 18;
  const int16_t badgeHeight = 25;
  const int16_t badgeX = x + width - badgeWidth - 13;
  const int16_t badgeY = y + height - badgeHeight - 12;
  display.drawRoundRect(
    badgeX,
    badgeY,
    badgeWidth,
    badgeHeight,
    badgeHeight / 2,
    GxEPD_RED
  );
  drawText(
    offlineLabel,
    badgeX + 9,
    badgeY + 18,
    &FreeSansBold9pt7b,
    GxEPD_RED
  );
}

void renderValue(
  JsonObjectConst widget,
  int16_t x,
  int16_t y,
  int16_t width,
  int16_t height
) {
  const bool available = widget["available"] | false;
  const String value = available
    ? limitedText(widget["value"] | "—", width < 300 ? 9 : 12)
    : "—";
  const String unit = limitedText(widget["unit"] | "", 8);
  const int16_t centerX = x + width / 2;
  const int16_t valueBaseline = y + max<int16_t>(92, height / 2 + 24);
  drawCentered(value, centerX, valueBaseline, &FreeSansBold24pt7b);
  if (available && !unit.isEmpty()) {
    drawCentered(unit, centerX, valueBaseline + 31, &FreeSans12pt7b);
  }
}

void renderStatus(
  JsonObjectConst widget,
  int16_t x,
  int16_t y,
  int16_t width,
  int16_t height
) {
  const bool available = widget["available"] | false;
  String value = limitedText(widget["value"] | "unknown", 14);
  value.toUpperCase();
  const bool active =
    value == "ON" || value == "OPEN" || value == "HOME" || value == "TRUE";
  const int16_t centerY = y + max<int16_t>(92, height / 2 + 12);
  const int16_t contentWidth = 54 + textWidth(value, &FreeSansBold18pt7b);
  const int16_t contentX = x + max<int16_t>(22, (width - contentWidth) / 2);
  display.drawCircle(contentX + 17, centerY - 8, 17, GxEPD_BLACK);
  if (available && active) {
    display.fillCircle(contentX + 17, centerY - 8, 12, GxEPD_RED);
  } else if (available) {
    display.fillCircle(contentX + 17, centerY - 8, 12, GxEPD_WHITE);
  }
  drawText(
    available ? value : "—",
    contentX + 54,
    centerY,
    &FreeSansBold18pt7b
  );
}

void renderGraph(
  JsonObjectConst widget,
  int16_t x,
  int16_t y,
  int16_t width,
  int16_t height
) {
  JsonArrayConst points = widget["points"].as<JsonArrayConst>();
  if (points.size() < 2) {
    drawCentered(
      "Waiting for data",
      x + width / 2,
      y + height / 2 + 12,
      &FreeSans12pt7b
    );
    return;
  }
  float minimum = points[0].as<float>();
  float maximum = minimum;
  for (JsonVariantConst point : points) {
    minimum = min(minimum, point.as<float>());
    maximum = max(maximum, point.as<float>());
  }
  if (maximum - minimum < 0.01F) {
    maximum = minimum + 1.0F;
  }
  const int16_t left = x + 20;
  const int16_t top = y + 62;
  const int16_t graphWidth = width - 40;
  const int16_t graphHeight = height - 94;
  for (uint8_t gridLine = 1; gridLine < 4; gridLine += 1) {
    const int16_t gridY = top + graphHeight * gridLine / 4;
    for (int16_t gridX = left; gridX < left + graphWidth; gridX += 12) {
      display.drawPixel(gridX, gridY, GxEPD_BLACK);
    }
  }
  drawText(String(maximum, 1), left, top - 8, &FreeSans9pt7b);
  drawText(String(minimum, 1), left, top + graphHeight + 17, &FreeSans9pt7b);
  int16_t previousX = left;
  int16_t previousY = top + graphHeight -
    static_cast<int16_t>((points[0].as<float>() - minimum) / (maximum - minimum) * graphHeight);
  for (size_t index = 1; index < points.size(); index += 1) {
    const int16_t pointX =
      left + static_cast<int16_t>(index * (graphWidth - 1) / (points.size() - 1));
    const int16_t pointY = top + graphHeight -
      static_cast<int16_t>((points[index].as<float>() - minimum) / (maximum - minimum) * graphHeight);
    display.drawLine(previousX, previousY, pointX, pointY, GxEPD_RED);
    display.drawLine(previousX, previousY + 1, pointX, pointY + 1, GxEPD_RED);
    previousX = pointX;
    previousY = pointY;
  }
}

void renderLayout(const String &payload) {
  JsonDocument document;
  if (deserializeJson(document, payload) != DeserializationError::Ok) {
    return;
  }
  JsonArrayConst widgets = document["widgets"].as<JsonArrayConst>();
  const uint8_t count = min(static_cast<uint8_t>(widgets.size()), maximumWidgets);
  uint8_t columns = 3;
  if (count <= 1) {
    columns = 1;
  } else if (count <= 4) {
    columns = 2;
  }
  const uint8_t rows = count <= 2 ? 1 : 2;
  const int16_t contentHeight = screenHeight - dashboardHeaderHeight;
  const int16_t cellWidth = screenWidth / columns;
  const int16_t cellHeight = contentHeight / rows;

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, screenWidth, 4, GxEPD_RED);
    drawText("SPACEPC", 14, 31, &FreeSansBold12pt7b);
    display.fillCircle(119, 24, 4, GxEPD_RED);
    drawText("HOME ASSISTANT DISPLAY", 134, 30, &FreeSans9pt7b);
    drawRightAligned(
      String(count) + (count == 1 ? " WIDGET" : " WIDGETS"),
      screenWidth - 14,
      30,
      &FreeSansBold9pt7b,
      GxEPD_RED
    );
    for (uint8_t index = 0; index < count; index += 1) {
      JsonObjectConst widget = widgets[index];
      const int16_t x = (index % columns) * cellWidth + cardGap / 2;
      const int16_t y =
        dashboardHeaderHeight +
        (index / columns) * cellHeight +
        cardGap / 2;
      const int16_t width = cellWidth - cardGap;
      const int16_t height = cellHeight - cardGap;
      display.drawRoundRect(x, y, width, height, cardRadius, GxEPD_BLACK);
      display.fillRoundRect(x + 13, y + 15, 5, 20, 2, GxEPD_RED);
      drawText(
        limitedText(widget["label"] | "Entity", width < 300 ? 18 : 28),
        x + 28,
        y + 33,
        &FreeSansBold12pt7b
      );
      const String type = widget["type"] | "value";
      if (type == "graph") {
        renderGraph(widget, x, y, width, height);
      } else if (type == "status") {
        renderStatus(widget, x, y, width, height);
      } else {
        renderValue(widget, x, y, width, height);
      }
      if (!(widget["available"] | false)) {
        renderOfflineBadge(x, y, width, height);
      }
    }
  } while (display.nextPage());
  lastRefresh = millis();
  currentPayload = payload;
}

bool acceptDisplayUpdate(const String &payload, String &errorMessage) {
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, payload);
  if (error) {
    errorMessage = "invalid_json";
    return false;
  }
  JsonArrayConst widgets = document["widgets"].as<JsonArrayConst>();
  if (widgets.isNull() || widgets.size() > maximumWidgets) {
    errorMessage = "invalid_widgets";
    return false;
  }
  for (JsonObjectConst widget : widgets) {
    const String type = widget["type"] | "";
    if (type != "value" && type != "status" && type != "graph") {
      errorMessage = "unsupported_widget";
      return false;
    }
    if (type == "graph" && widget["points"].as<JsonArrayConst>().size() > maximumGraphPoints) {
      errorMessage = "too_many_graph_points";
      return false;
    }
  }
  pendingPayload = payload;
  return true;
}

void renderStartup() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, 12, screenHeight, GxEPD_RED);
    drawText("SPACEPC", 48, 84, &FreeSansBold24pt7b);
    display.fillCircle(244, 70, 7, GxEPD_RED);
    drawText("HOME ASSISTANT DISPLAY", 49, 128, &FreeSansBold12pt7b, GxEPD_RED);
    display.drawLine(49, 154, 750, 154, GxEPD_BLACK);
    drawText("Ready for configuration", 49, 224, &FreeSansBold18pt7b);
    drawText(
      "Add this device in Home Assistant, then choose",
      49,
      271,
      &FreeSans12pt7b
    );
    drawText(
      "entities and widgets under Configure.",
      49,
      306,
      &FreeSans12pt7b
    );
    drawText("Local only  /  spacepc.dev", 49, 428, &FreeSans9pt7b);
  } while (display.nextPage());
}
}

void setup() {
  Serial.begin(115200);
  delay(250);
  display.init(115200, true, 2, false);
  display.setRotation(0);
  displayReady = true;
  renderStartup();
  portal.enableDisplayApi(displayCapabilities, acceptDisplayUpdate);
  portal.begin(projectConfig);
  portal.setProjectStatus("waiting for Home Assistant");
}

void loop() {
  portal.loop();
  if (
    displayReady &&
    !pendingPayload.isEmpty() &&
    (lastRefresh == 0 || millis() - lastRefresh >= minimumRefreshMs)
  ) {
    const String payload = pendingPayload;
    pendingPayload = "";
    renderLayout(payload);
    portal.setProjectStatus("display updated");
  }
}
