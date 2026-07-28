#include <Arduino.h>
#include <DallasTemperature.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <OneWire.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>

#include "settings.h"
#include "web_page.h"

namespace {
constexpr uint32_t wifiConnectTimeoutMs = 20000;
constexpr uint32_t mqttRetryIntervalMs = 5000;
constexpr char setupAccessPointPassword[] = "spacepcsetup";

DeviceSettings settings;
WiFiClient networkClient;
PubSubClient mqttClient(networkClient);
WebServer webServer(80);
DNSServer dnsServer;
OneWire *oneWire = nullptr;
DallasTemperature *temperatureSensors = nullptr;

String identifier;
String setupAccessPointName;
bool accessPointActive = false;
bool sensorConnected = false;
uint32_t lastMqttAttempt = 0;
uint32_t lastPublish = 0;

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}

String jsonEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "\\r");
  value.replace("\t", "\\t");
  return value;
}

String normalizedTopic(String topic) {
  topic.trim();
  while (topic.endsWith("/")) {
    topic.remove(topic.length() - 1);
  }
  return topic;
}

bool validPublishTopic(const String &topic) {
  return !topic.isEmpty() &&
    topic.length() <= 128 &&
    topic.indexOf('#') < 0 &&
    topic.indexOf('+') < 0;
}

String stateTopic() {
  return normalizedTopic(settings.mqttBaseTopic) + "/state";
}

String availabilityTopic() {
  return normalizedTopic(settings.mqttBaseTopic) + "/availability";
}

String discoveryTopic() {
  return "homeassistant/sensor/" + identifier + "/temperature/config";
}

bool validPin(int pin) {
  return pin >= 0 && pin <= 48;
}

void initializeSensor() {
  delete temperatureSensors;
  delete oneWire;
  oneWire = new OneWire(settings.sensorPin);
  temperatureSensors = new DallasTemperature(oneWire);
  temperatureSensors->begin();
  sensorConnected = temperatureSensors->getDeviceCount() > 0;
}

void startSetupAccessPoint() {
  if (accessPointActive) {
    return;
  }

  WiFi.mode(WIFI_AP_STA);
  accessPointActive = WiFi.softAP(
    setupAccessPointName.c_str(),
    setupAccessPointPassword
  );
  if (accessPointActive) {
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.printf(
      "Setup access point: %s, password: %s, address: http://%s\n",
      setupAccessPointName.c_str(),
      setupAccessPointPassword,
      WiFi.softAPIP().toString().c_str()
    );
  }
}

void connectWifi() {
  if (settings.wifiSsid.isEmpty()) {
    startSetupAccessPoint();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(identifier.c_str());
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
  const uint32_t startedAt = millis();
  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - startedAt < wifiConnectTimeoutMs
  ) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(
      "Wi-Fi connected: http://%s or http://%s.local\n",
      WiFi.localIP().toString().c_str(),
      identifier.c_str()
    );
    MDNS.begin(identifier.c_str());
    return;
  }

  Serial.println("Wi-Fi connection failed; starting configuration access point.");
  startSetupAccessPoint();
}

String renderSettingsPage() {
  String page(settingsPage);
  page.replace("{{DEVICE_NAME}}", htmlEscape(settings.deviceName));
  page.replace("{{SENSOR_PIN}}", String(settings.sensorPin));
  page.replace("{{INTERVAL}}", String(settings.publishIntervalSeconds));
  page.replace("{{WIFI_SSID}}", htmlEscape(settings.wifiSsid));
  page.replace("{{MQTT_HOST}}", htmlEscape(settings.mqttHost));
  page.replace("{{MQTT_PORT}}", String(settings.mqttPort));
  page.replace("{{MQTT_USERNAME}}", htmlEscape(settings.mqttUsername));
  page.replace("{{MQTT_TOPIC}}", htmlEscape(settings.mqttBaseTopic));
  page.replace(
    "{{HA_CHECKED}}",
    settings.homeAssistantDiscovery ? "checked" : ""
  );
  return page;
}

void redirectToPortal() {
  webServer.sendHeader("Location", "/", true);
  webServer.send(302, "text/plain", "");
}

String requiredArgument(const char *name) {
  return webServer.hasArg(name) ? webServer.arg(name) : "";
}

void handleSave() {
  DeviceSettings next = settings;
  next.deviceName = requiredArgument("deviceName");
  next.wifiSsid = requiredArgument("wifiSsid");
  next.sensorPin = requiredArgument("sensorPin").toInt();
  next.mqttHost = requiredArgument("mqttHost");
  next.mqttPort = static_cast<uint16_t>(requiredArgument("mqttPort").toInt());
  next.mqttUsername = requiredArgument("mqttUsername");
  next.mqttBaseTopic = normalizedTopic(requiredArgument("mqttBaseTopic"));
  next.homeAssistantDiscovery = webServer.hasArg("homeAssistantDiscovery");
  next.publishIntervalSeconds = requiredArgument("interval").toInt();

  const String wifiPassword = requiredArgument("wifiPassword");
  const String mqttPassword = requiredArgument("mqttPassword");
  if (webServer.hasArg("clearWifiPassword")) {
    next.wifiPassword = "";
  } else if (!wifiPassword.isEmpty()) {
    next.wifiPassword = wifiPassword;
  }
  if (webServer.hasArg("clearMqttPassword")) {
    next.mqttPassword = "";
  } else if (!mqttPassword.isEmpty()) {
    next.mqttPassword = mqttPassword;
  }

  next.deviceName.trim();
  next.wifiSsid.trim();
  next.mqttHost.trim();
  next.mqttUsername.trim();

  const bool invalid =
    next.deviceName.isEmpty() ||
    next.wifiSsid.isEmpty() ||
    !validPin(next.sensorPin) ||
    next.mqttHost.isEmpty() ||
    next.mqttPort == 0 ||
    !validPublishTopic(next.mqttBaseTopic) ||
    next.publishIntervalSeconds < 5 ||
    next.publishIntervalSeconds > 86400;
  if (invalid) {
    webServer.send(
      400,
      "text/plain",
      "Invalid settings. Check all required fields and value ranges."
    );
    return;
  }

  if (!saveSettings(next)) {
    webServer.send(500, "text/plain", "Could not store settings.");
    return;
  }

  webServer.send(
    200,
    "text/html",
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<p>Settings saved. The device is restarting…</p>"
  );
  delay(500);
  ESP.restart();
}

void handleStatus() {
  String payload = "{";
  payload += "\"sensor\":\"";
  payload += sensorConnected ? "connected" : "not found";
  payload += "\",\"wifi\":\"";
  payload += WiFi.status() == WL_CONNECTED ? "connected" : "setup access point";
  payload += "\",\"mqtt\":\"";
  payload += mqttClient.connected() ? "connected" : "disconnected";
  payload += "\",\"ip\":\"";
  payload += WiFi.status() == WL_CONNECTED
    ? WiFi.localIP().toString()
    : WiFi.softAPIP().toString();
  payload += "\"}";
  webServer.send(200, "application/json", payload);
}

void startWebServer() {
  webServer.on("/", HTTP_GET, [] {
    webServer.send(200, "text/html; charset=utf-8", renderSettingsPage());
  });
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.on("/api/status", HTTP_GET, handleStatus);
  webServer.on("/generate_204", HTTP_ANY, redirectToPortal);
  webServer.on("/hotspot-detect.html", HTTP_ANY, redirectToPortal);
  webServer.on("/connecttest.txt", HTTP_ANY, redirectToPortal);
  webServer.onNotFound(redirectToPortal);
  webServer.begin();
}

void publishHomeAssistantDiscovery() {
  if (!settings.homeAssistantDiscovery) {
    mqttClient.publish(discoveryTopic().c_str(), "", true);
    return;
  }

  String payload = "{";
  payload += "\"name\":\"Temperature\",";
  payload += "\"unique_id\":\"" + identifier + "_temperature\",";
  payload += "\"state_topic\":\"" + stateTopic() + "\",";
  payload += "\"availability_topic\":\"" + availabilityTopic() + "\",";
  payload += "\"value_template\":\"{{ value_json.temperature }}\",";
  payload += "\"device_class\":\"temperature\",";
  payload += "\"state_class\":\"measurement\",";
  payload += "\"unit_of_measurement\":\"°C\",";
  payload += "\"device\":{";
  payload += "\"identifiers\":[\"" + identifier + "\"],";
  payload += "\"name\":\"" + jsonEscape(settings.deviceName) + "\",";
  payload += "\"manufacturer\":\"SpacePC\",";
  payload += "\"model\":\"ESP32 DS18B20 MQTT Sensor\"";
  payload += "}}";
  mqttClient.publish(discoveryTopic().c_str(), payload.c_str(), true);
}

bool connectMqtt() {
  if (
    WiFi.status() != WL_CONNECTED ||
    settings.mqttHost.isEmpty() ||
    mqttClient.connected()
  ) {
    return mqttClient.connected();
  }

  mqttClient.setServer(settings.mqttHost.c_str(), settings.mqttPort);
  const bool connected = settings.mqttUsername.isEmpty()
    ? mqttClient.connect(
        identifier.c_str(),
        availabilityTopic().c_str(),
        1,
        true,
        "offline"
      )
    : mqttClient.connect(
        identifier.c_str(),
        settings.mqttUsername.c_str(),
        settings.mqttPassword.c_str(),
        availabilityTopic().c_str(),
        1,
        true,
        "offline"
      );

  if (connected) {
    mqttClient.publish(availabilityTopic().c_str(), "online", true);
    publishHomeAssistantDiscovery();
  }
  return connected;
}

void publishTemperature() {
  if (!temperatureSensors || !mqttClient.connected()) {
    return;
  }

  temperatureSensors->requestTemperatures();
  const float temperature = temperatureSensors->getTempCByIndex(0);
  sensorConnected =
    temperature != DEVICE_DISCONNECTED_C &&
    temperature >= -55.0F &&
    temperature <= 125.0F;
  if (!sensorConnected) {
    Serial.println("DS18B20 reading failed.");
    return;
  }

  char payload[48];
  snprintf(payload, sizeof(payload), "{\"temperature\":%.2f}", temperature);
  mqttClient.publish(stateTopic().c_str(), payload, true);
  Serial.printf("Published temperature: %.2f °C\n", temperature);
}
}

void setup() {
  Serial.begin(115200);
  delay(250);
  identifier = deviceIdentifier();
  setupAccessPointName = "SpacePC-Temp-" + identifier.substring(identifier.length() - 6);
  loadSettings(settings);
  initializeSensor();
  connectWifi();
  startWebServer();
}

void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();
  mqttClient.loop();

  const uint32_t now = millis();
  if (
    !mqttClient.connected() &&
    now - lastMqttAttempt >= mqttRetryIntervalMs
  ) {
    lastMqttAttempt = now;
    connectMqtt();
  }

  const uint32_t publishIntervalMs = settings.publishIntervalSeconds * 1000UL;
  if (
    mqttClient.connected() &&
    (lastPublish == 0 || now - lastPublish >= publishIntervalMs)
  ) {
    lastPublish = now;
    publishTemperature();
  }
}
