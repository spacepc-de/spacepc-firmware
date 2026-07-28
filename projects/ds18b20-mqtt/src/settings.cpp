#include "settings.h"

#include <Preferences.h>
#include <WiFi.h>

namespace {
constexpr char preferencesNamespace[] = "spacepc-temp";
}

void loadSettings(DeviceSettings &settings) {
  Preferences preferences;
  if (!preferences.begin(preferencesNamespace, true)) {
    return;
  }

  settings.deviceName = preferences.getString("name", settings.deviceName);
  settings.wifiSsid = preferences.getString("wifi-ssid", "");
  settings.wifiPassword = preferences.getString("wifi-pass", "");
  settings.sensorPin = preferences.getInt("sensor-pin", settings.sensorPin);
  settings.mqttHost = preferences.getString("mqtt-host", "");
  settings.mqttPort = preferences.getUShort("mqtt-port", settings.mqttPort);
  settings.mqttUsername = preferences.getString("mqtt-user", "");
  settings.mqttPassword = preferences.getString("mqtt-pass", "");
  settings.mqttBaseTopic = preferences.getString("mqtt-topic", settings.mqttBaseTopic);
  settings.homeAssistantDiscovery = preferences.getBool(
    "ha-discovery",
    settings.homeAssistantDiscovery
  );
  settings.publishIntervalSeconds = preferences.getUInt(
    "interval",
    settings.publishIntervalSeconds
  );
  preferences.end();
}

bool saveSettings(const DeviceSettings &settings) {
  Preferences preferences;
  if (!preferences.begin(preferencesNamespace, false)) {
    return false;
  }

  bool success = true;
  success &= preferences.putString("name", settings.deviceName) > 0;
  success &= preferences.putString("wifi-ssid", settings.wifiSsid) > 0;
  preferences.putString("wifi-pass", settings.wifiPassword);
  success &= preferences.putInt("sensor-pin", settings.sensorPin) > 0;
  success &= preferences.putString("mqtt-host", settings.mqttHost) > 0;
  success &= preferences.putUShort("mqtt-port", settings.mqttPort) > 0;
  preferences.putString("mqtt-user", settings.mqttUsername);
  preferences.putString("mqtt-pass", settings.mqttPassword);
  success &= preferences.putString("mqtt-topic", settings.mqttBaseTopic) > 0;
  success &= preferences.putBool(
    "ha-discovery",
    settings.homeAssistantDiscovery
  ) > 0;
  success &= preferences.putUInt(
    "interval",
    settings.publishIntervalSeconds
  ) > 0;
  preferences.end();
  return success;
}

String deviceIdentifier() {
  const uint64_t chipId = ESP.getEfuseMac();
  char identifier[24];
  snprintf(
    identifier,
    sizeof(identifier),
    "spacepc-%04x%08x",
    static_cast<uint16_t>(chipId >> 32),
    static_cast<uint32_t>(chipId)
  );
  return String(identifier);
}
