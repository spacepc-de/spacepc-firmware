#include <Arduino.h>
#include <ArduinoJson.h>
#include <GxEPD2_3C.h>
#include <SpacePCPortal.h>

#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold24pt7b.h>

namespace {
constexpr uint16_t screenWidth = 800;
constexpr uint16_t screenHeight = 480;
constexpr uint8_t maximumWidgets = 6;
constexpr uint8_t maximumGraphPoints = 48;
constexpr uint32_t minimumRefreshMs = 300000;

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
  "0.1.0",
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

void renderValue(JsonObjectConst widget, int16_t x, int16_t y, int16_t width) {
  const bool available = widget["available"] | false;
  String value = available ? limitedText(widget["value"] | "—", 12) : "—";
  String unit = limitedText(widget["unit"] | "", 8);
  drawText(value, x + 18, y + 84, &FreeMonoBold24pt7b);
  if (!unit.isEmpty()) {
    drawText(unit, x + 18, y + 116, &FreeMonoBold12pt7b);
  }
  if (!available) {
    drawText("OFFLINE", x + width - 112, y + 30, &FreeMonoBold12pt7b, GxEPD_RED);
  }
}

void renderStatus(JsonObjectConst widget, int16_t x, int16_t y) {
  const bool available = widget["available"] | false;
  String value = limitedText(widget["value"] | "unknown", 14);
  value.toUpperCase();
  const bool active =
    value == "ON" || value == "OPEN" || value == "HOME" || value == "TRUE";
  display.fillCircle(x + 42, y + 82, 20, available && active ? GxEPD_RED : GxEPD_BLACK);
  drawText(available ? value : "OFFLINE", x + 78, y + 91, &FreeMonoBold18pt7b);
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
    drawText("Waiting for data", x + 18, y + 84, &FreeMonoBold12pt7b);
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
  const int16_t left = x + 18;
  const int16_t top = y + 52;
  const int16_t graphWidth = width - 36;
  const int16_t graphHeight = height - 72;
  display.drawRect(left, top, graphWidth, graphHeight, GxEPD_BLACK);
  int16_t previousX = left;
  int16_t previousY = top + graphHeight -
    static_cast<int16_t>((points[0].as<float>() - minimum) / (maximum - minimum) * graphHeight);
  for (size_t index = 1; index < points.size(); index += 1) {
    const int16_t pointX =
      left + static_cast<int16_t>(index * (graphWidth - 1) / (points.size() - 1));
    const int16_t pointY = top + graphHeight -
      static_cast<int16_t>((points[index].as<float>() - minimum) / (maximum - minimum) * graphHeight);
    display.drawLine(previousX, previousY, pointX, pointY, GxEPD_RED);
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
  uint8_t columns = constrain(document["layout"]["columns"] | 2, 1, 3);
  const uint8_t count = min(static_cast<uint8_t>(widgets.size()), maximumWidgets);
  const uint8_t rows = max(static_cast<uint8_t>(1), static_cast<uint8_t>((count + columns - 1) / columns));
  const int16_t cellWidth = screenWidth / columns;
  const int16_t cellHeight = screenHeight / rows;

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    for (uint8_t index = 0; index < count; index += 1) {
      JsonObjectConst widget = widgets[index];
      const int16_t x = (index % columns) * cellWidth;
      const int16_t y = (index / columns) * cellHeight;
      display.drawRect(x + 4, y + 4, cellWidth - 8, cellHeight - 8, GxEPD_BLACK);
      drawText(limitedText(widget["label"] | "Entity", 22), x + 18, y + 34, &FreeMonoBold12pt7b);
      const String type = widget["type"] | "value";
      if (type == "graph") {
        renderGraph(widget, x, y, cellWidth, cellHeight);
      } else if (type == "status") {
        renderStatus(widget, x, y);
      } else {
        renderValue(widget, x, y, cellWidth);
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
    drawText("SpacePC.dev", 34, 72, &FreeMonoBold24pt7b, GxEPD_RED);
    drawText("Home Assistant Display", 34, 126, &FreeMonoBold18pt7b);
    drawText("Waiting for Home Assistant configuration", 34, 192, &FreeMonoBold12pt7b);
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
