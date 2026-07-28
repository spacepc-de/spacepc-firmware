#pragma once

#include <Arduino.h>

struct DeviceSettings {
  String deviceName = "SpacePC Temperature";
  String wifiSsid;
  String wifiPassword;
  int sensorPin = 4;
  String mqttHost;
  uint16_t mqttPort = 1883;
  String mqttUsername;
  String mqttPassword;
  String mqttBaseTopic = "spacepc/temperature";
  bool homeAssistantDiscovery = true;
  uint32_t publishIntervalSeconds = 60;
};

void loadSettings(DeviceSettings &settings);
bool saveSettings(const DeviceSettings &settings);
String deviceIdentifier();
