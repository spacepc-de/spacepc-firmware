// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SpacePC Weather Display contributors
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "AppConfig.h"
#include "ConfigStore.h"
#include "DisplayRenderer.h"
#include "ProvisioningPortal.h"
#include "WeatherClient.h"

namespace {
constexpr uint64_t MICROSECONDS_PER_MINUTE = 60ULL * 1000000ULL;

AppConfig config;
ConfigStore configStore;
ProvisioningPortal portal;
WeatherClient weatherClient;
DisplayRenderer renderer;

void sleepFor(uint16_t minutes) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  renderer.powerOff();
  Serial.printf("Deep sleep for %u minutes\n", minutes);
  esp_sleep_enable_timer_wakeup((uint64_t)minutes * MICROSECONDS_PER_MINUTE);
  Serial.flush();
  esp_deep_sleep_start();
}
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nSpacePC Weather Display boot");

  configStore.load(config);
  renderer.begin();
  const String accessPoint =
      "SpacePC-Weather-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  const bool forcePortal = !config.weatherReady();
  const auto showPortalInstructions = [](const String& activeAccessPoint) {
    renderer.showSetup(activeAccessPoint);
  };
  if (!portal.connectAndConfigure(config, forcePortal, showPortalInstructions)) {
    Serial.println("Provisioning timed out");
    renderer.showError(
        "Wi-Fi setup timed out",
        "The display could not connect to a wireless network.",
        "1  Join " + accessPoint +
        "\n2  Complete the setup page"
        "\n3  Keep this display powered while configuring");
    sleepFor(5);
  }
  configStore.save(config);

  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");

  WeatherData weather;
  if (!weatherClient.fetch(config, weather)) {
    renderer.showError(
        "Weather data unavailable",
        weather.error,
        "1  Join " + accessPoint +
        "\n2  Check location and OpenWeatherMap API key"
        "\n3  New API keys can take a few hours to activate");
    if (!portal.connectAndConfigure(config, true, showPortalInstructions)) {
      sleepFor(5);
    }
    configStore.save(config);
    if (!weatherClient.fetch(config, weather)) {
      renderer.showError(
          "Weather data still unavailable",
          weather.error,
          "Check the API key and location in " + accessPoint +
          "\nOpenWeatherMap keys may need time to activate"
          "\nThe display will retry in five minutes");
      sleepFor(5);
    }
  }

  renderer.showDashboard(weather, WiFi.RSSI());
  sleepFor(config.refreshMinutes);
}

void loop() {}
