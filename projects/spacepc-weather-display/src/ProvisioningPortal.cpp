// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProvisioningPortal.h"

#include <WiFiManager.h>

#include "PortalTheme.h"

namespace {
void copyValue(const char* value, String& target) {
  if (value != nullptr) target = String(value);
}
}

bool ProvisioningPortal::connectAndConfigure(
    AppConfig& config, bool forcePortal,
    std::function<void(const String&)> onPortalStarted) {
  WiFiManager manager;
  manager.setConfigPortalTimeout(300);
  manager.setConnectTimeout(20);
  manager.setClass("invert");
  manager.setTitle("SpacePC Weather Display");
  manager.setCustomHeadElement(PortalTheme::head);
  manager.setShowInfoErase(false);
  manager.setShowInfoUpdate(false);
  manager.setAPCallback([onPortalStarted](WiFiManager* activeManager) {
    if (onPortalStarted) onPortalStarted(activeManager->getConfigPortalSSID());
  });

  char refreshBuffer[6];
  snprintf(refreshBuffer, sizeof(refreshBuffer), "%u", config.refreshMinutes);

  WiFiManagerParameter header(PortalTheme::header);
  WiFiManagerParameter deviceSection(PortalTheme::deviceSection);
  WiFiManagerParameter refresh("refresh", "Aktualisierung in Minuten", refreshBuffer, 5);
  WiFiManagerParameter weatherSection(PortalTheme::weatherSection);
  WiFiManagerParameter owmKey(
      "owm_key", "OpenWeatherMap API Key", config.owmApiKey.c_str(), 64,
      "type='password' autocomplete='off'");
  WiFiManagerParameter weatherLocation(
      "owm_city", "Ort (z. B. Berlin)", config.weatherLocation.c_str(), 64);
  WiFiManagerParameter countryCode(
      "owm_country", "Ländercode (z. B. DE)", config.countryCode.c_str(), 3);
  WiFiManagerParameter footer(PortalTheme::footer);

  manager.addParameter(&header);
  manager.addParameter(&deviceSection);
  manager.addParameter(&refresh);
  manager.addParameter(&weatherSection);
  manager.addParameter(&owmKey);
  manager.addParameter(&weatherLocation);
  manager.addParameter(&countryCode);
  manager.addParameter(&footer);

  const String accessPoint =
      "SpacePC-Weather-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool connected = forcePortal
      ? manager.startConfigPortal(accessPoint.c_str())
      : manager.autoConnect(accessPoint.c_str());
  if (!connected) return false;

  copyValue(owmKey.getValue(), config.owmApiKey);
  copyValue(weatherLocation.getValue(), config.weatherLocation);
  config.weatherLocation.trim();
  copyValue(countryCode.getValue(), config.countryCode);
  config.countryCode.trim();
  config.countryCode.toUpperCase();
  config.refreshMinutes = constrain(String(refresh.getValue()).toInt(), 5, 1440);
  return true;
}
