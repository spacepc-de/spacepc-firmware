#include "SpacePCPortal.h"

#include <ESPmDNS.h>
#include <Preferences.h>

#include "WebTemplate.h"

namespace {
constexpr uint32_t wifiConnectTimeoutMs = 20000;
constexpr uint32_t wifiRetryIntervalMs = 10000;
constexpr uint32_t wifiAccessPointFallbackMs = 60000;
constexpr uint32_t mqttRetryIntervalMs = 5000;
constexpr char setupAccessPointPassword[] = "spacepcsetup";
constexpr char localApiPath[] = "/api/v1";

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

bool validFieldKey(const char *key) {
  if (!key) {
    return false;
  }
  const size_t length = strlen(key);
  if (length == 0 || length > 12) {
    return false;
  }
  for (size_t index = 0; index < length; index += 1) {
    const char character = key[index];
    if (
      !isAlphaNumeric(character) &&
      character != '-' &&
      character != '_'
    ) {
      return false;
    }
  }
  return true;
}

bool validEntityId(const char *entityId) {
  if (!entityId) {
    return false;
  }
  const size_t length = strlen(entityId);
  if (length == 0 || length > 64) {
    return false;
  }
  for (size_t index = 0; index < length; index += 1) {
    const char character = entityId[index];
    if (
      !isAlphaNumeric(character) &&
      character != '-' &&
      character != '_'
    ) {
      return false;
    }
  }
  return true;
}

bool validEntityPlatform(const char *platform) {
  if (!platform || strlen(platform) == 0) {
    return true;
  }
  return
    strcmp(platform, "sensor") == 0 ||
    strcmp(platform, "binary_sensor") == 0 ||
    strcmp(platform, "switch") == 0 ||
    strcmp(platform, "light") == 0 ||
    strcmp(platform, "fan") == 0 ||
    strcmp(platform, "update") == 0;
}

String preferenceKey(const char *fieldKey) {
  return "f-" + String(fieldKey);
}

String generatedDeviceId() {
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
}

SpacePCPortal::SpacePCPortal()
  : config_{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
    mqttClient_(networkClient_),
    webServer_(80),
    numberFieldCount_(0),
    textFieldCount_(0),
    checkboxFieldCount_(0),
    entityCount_(0),
    mqttPort_(1883),
    homeAssistantDiscovery_(true),
    publishIntervalSeconds_(60),
    projectStatus_("starting"),
    displayUpdateHandler_(nullptr),
    accessPointActive_(false),
    wifiConnected_(false),
    mdnsActive_(false),
    lastWifiAttempt_(0),
    wifiDisconnectedSince_(0),
    lastMqttAttempt_(0),
    lastPublish_(0) {}

void SpacePCPortal::enableDisplayApi(
  const String &capabilitiesJson,
  SpacePCDisplayUpdateHandler updateHandler
) {
  displayCapabilitiesJson_ = capabilitiesJson;
  displayUpdateHandler_ = updateHandler;
}

bool SpacePCPortal::addNumberField(const SpacePCNumberField &field) {
  if (
    numberFieldCount_ >= maxFields ||
    !validFieldKey(field.key) ||
    fieldKeyExists(field.key) ||
    field.minimum > field.maximum ||
    field.defaultValue < field.minimum ||
    field.defaultValue > field.maximum
  ) {
    return false;
  }
  numberFields_[numberFieldCount_].definition = field;
  numberFields_[numberFieldCount_].value = field.defaultValue;
  numberFieldCount_ += 1;
  return true;
}

bool SpacePCPortal::addTextField(const SpacePCTextField &field) {
  if (
    textFieldCount_ >= maxFields ||
    !validFieldKey(field.key) ||
    fieldKeyExists(field.key) ||
    !field.defaultValue ||
    field.maximumLength == 0 ||
    field.maximumLength > 256
  ) {
    return false;
  }
  textFields_[textFieldCount_].definition = field;
  textFields_[textFieldCount_].value = field.defaultValue;
  textFieldCount_ += 1;
  return true;
}

bool SpacePCPortal::addCheckboxField(const SpacePCCheckboxField &field) {
  if (
    checkboxFieldCount_ >= maxFields ||
    !validFieldKey(field.key) ||
    fieldKeyExists(field.key)
  ) {
    return false;
  }
  checkboxFields_[checkboxFieldCount_].definition = field;
  checkboxFields_[checkboxFieldCount_].value = field.defaultValue;
  checkboxFieldCount_ += 1;
  return true;
}

bool SpacePCPortal::addHomeAssistantEntity(
  const SpacePCHomeAssistantEntity &entity
) {
  if (
    entityCount_ >= maxEntities ||
    !validEntityId(entity.objectId) ||
    !entity.name ||
    !entity.valueTemplate ||
    !validEntityPlatform(entity.platform)
  ) {
    return false;
  }
  entities_[entityCount_] = entity;
  entityValues_[entityCount_] = "null";
  entityAvailable_[entityCount_] = false;
  entityCount_ += 1;
  return true;
}

void SpacePCPortal::begin(const SpacePCPortalConfig &config) {
  config_ = config;
  deviceId_ = generatedDeviceId();
  settingsNamespace_ =
    config_.projectId && strlen(config_.projectId) <= 15
      ? config_.projectId
      : "spacepc";
  accessPointName_ =
    "SpacePC-" +
    String(config_.projectId ? config_.projectId : "Setup") +
    "-" +
    deviceId_.substring(deviceId_.length() - 6);
  if (accessPointName_.length() > 31) {
    accessPointName_ =
      accessPointName_.substring(0, 24) +
      "-" +
      deviceId_.substring(deviceId_.length() - 6);
  }

  loadSettings();
  mqttClient_.setBufferSize(1024);
  connectWifi();
  startWebServer();
}

void SpacePCPortal::loop() {
  maintainWifi();
  if (accessPointActive_) {
    dnsServer_.processNextRequest();
  }
  webServer_.handleClient();
  mqttClient_.loop();

  const uint32_t now = millis();
  if (
    !mqttClient_.connected() &&
    now - lastMqttAttempt_ >= mqttRetryIntervalMs
  ) {
    lastMqttAttempt_ = now;
    connectMqtt();
  }
}

int SpacePCPortal::numberValue(const char *key) const {
  if (!key) {
    return 0;
  }
  for (size_t index = 0; index < numberFieldCount_; index += 1) {
    if (strcmp(numberFields_[index].definition.key, key) == 0) {
      return numberFields_[index].value;
    }
  }
  return 0;
}

String SpacePCPortal::textValue(const char *key) const {
  if (!key) {
    return "";
  }
  for (size_t index = 0; index < textFieldCount_; index += 1) {
    if (strcmp(textFields_[index].definition.key, key) == 0) {
      return textFields_[index].value;
    }
  }
  return "";
}

bool SpacePCPortal::checkboxValue(const char *key) const {
  if (!key) {
    return false;
  }
  for (size_t index = 0; index < checkboxFieldCount_; index += 1) {
    if (strcmp(checkboxFields_[index].definition.key, key) == 0) {
      return checkboxFields_[index].value;
    }
  }
  return false;
}

bool SpacePCPortal::mqttConnected() {
  return mqttClient_.connected();
}

bool SpacePCPortal::publishDue() {
  const uint32_t intervalMs = publishIntervalSeconds_ * 1000UL;
  return lastPublish_ == 0 || millis() - lastPublish_ >= intervalMs;
}

bool SpacePCPortal::publishState(const String &jsonPayload) {
  if (jsonPayload.isEmpty()) {
    return false;
  }
  lastPublish_ = millis();
  if (!mqttClient_.connected()) {
    return false;
  }
  const bool published = mqttClient_.publish(
    stateTopic().c_str(),
    jsonPayload.c_str(),
    true
  );
  return published;
}

bool SpacePCPortal::setEntityState(
  const char *objectId,
  const String &jsonValue,
  bool available
) {
  const int index = entityIndex(objectId);
  if (index < 0 || jsonValue.isEmpty()) {
    return false;
  }
  entityValues_[index] = jsonValue;
  entityAvailable_[index] = available;
  return true;
}

bool SpacePCPortal::setEntityUnavailable(const char *objectId) {
  const int index = entityIndex(objectId);
  if (index < 0) {
    return false;
  }
  entityValues_[index] = "null";
  entityAvailable_[index] = false;
  return true;
}

void SpacePCPortal::setProjectStatus(const String &status) {
  projectStatus_ = status;
}

String SpacePCPortal::deviceId() const {
  return deviceId_;
}

String SpacePCPortal::configurationUrl() const {
  if (WiFi.status() == WL_CONNECTED) {
    return "http://" + WiFi.localIP().toString();
  }
  return "http://" + WiFi.softAPIP().toString();
}

void SpacePCPortal::loadSettings() {
  deviceName_ = config_.displayName ? config_.displayName : "SpacePC device";
  mqttBaseTopic_ =
    "spacepc/" +
    String(config_.projectId ? config_.projectId : "device") +
    "/" +
    deviceId_;

  Preferences preferences;
  if (!preferences.begin(settingsNamespace_.c_str(), true)) {
    return;
  }
  deviceName_ = preferences.getString("name", deviceName_);
  wifiSsid_ = preferences.getString("wifi-ssid", "");
  wifiPassword_ = preferences.getString("wifi-pass", "");
  mqttHost_ = preferences.getString("mqtt-host", "");
  mqttPort_ = preferences.getUShort("mqtt-port", mqttPort_);
  mqttUsername_ = preferences.getString("mqtt-user", "");
  mqttPassword_ = preferences.getString("mqtt-pass", "");
  mqttBaseTopic_ = preferences.getString("mqtt-topic", mqttBaseTopic_);
  homeAssistantDiscovery_ = preferences.getBool(
    "ha-discovery",
    homeAssistantDiscovery_
  );
  publishIntervalSeconds_ = preferences.getUInt(
    "interval",
    publishIntervalSeconds_
  );

  for (size_t index = 0; index < numberFieldCount_; index += 1) {
    NumberFieldState &field = numberFields_[index];
    field.value = preferences.getInt(
      preferenceKey(field.definition.key).c_str(),
      field.definition.defaultValue
    );
  }
  for (size_t index = 0; index < textFieldCount_; index += 1) {
    TextFieldState &field = textFields_[index];
    field.value = preferences.getString(
      preferenceKey(field.definition.key).c_str(),
      field.definition.defaultValue
    );
  }
  for (size_t index = 0; index < checkboxFieldCount_; index += 1) {
    CheckboxFieldState &field = checkboxFields_[index];
    field.value = preferences.getBool(
      preferenceKey(field.definition.key).c_str(),
      field.definition.defaultValue
    );
  }
  preferences.end();
}

bool SpacePCPortal::saveSettings() {
  Preferences preferences;
  if (!preferences.begin(settingsNamespace_.c_str(), false)) {
    return false;
  }

  bool success = true;
  success &= preferences.putString("name", deviceName_) > 0;
  success &= preferences.putString("wifi-ssid", wifiSsid_) > 0;
  preferences.putString("wifi-pass", wifiPassword_);
  preferences.putString("mqtt-host", mqttHost_);
  success &= preferences.putUShort("mqtt-port", mqttPort_) > 0;
  preferences.putString("mqtt-user", mqttUsername_);
  preferences.putString("mqtt-pass", mqttPassword_);
  success &= preferences.putString("mqtt-topic", mqttBaseTopic_) > 0;
  success &= preferences.putBool(
    "ha-discovery",
    homeAssistantDiscovery_
  ) > 0;
  success &= preferences.putUInt(
    "interval",
    publishIntervalSeconds_
  ) > 0;

  for (size_t index = 0; index < numberFieldCount_; index += 1) {
    NumberFieldState &field = numberFields_[index];
    success &= preferences.putInt(
      preferenceKey(field.definition.key).c_str(),
      field.value
    ) > 0;
  }
  for (size_t index = 0; index < textFieldCount_; index += 1) {
    TextFieldState &field = textFields_[index];
    preferences.putString(
      preferenceKey(field.definition.key).c_str(),
      field.value
    );
  }
  for (size_t index = 0; index < checkboxFieldCount_; index += 1) {
    CheckboxFieldState &field = checkboxFields_[index];
    success &= preferences.putBool(
      preferenceKey(field.definition.key).c_str(),
      field.value
    ) > 0;
  }
  preferences.end();
  return success;
}

void SpacePCPortal::connectWifi() {
  if (wifiSsid_.isEmpty()) {
    startAccessPoint();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceId_.c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid_.c_str(), wifiPassword_.c_str());
  lastWifiAttempt_ = millis();
  const uint32_t startedAt = millis();
  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - startedAt < wifiConnectTimeoutMs
  ) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected_ = true;
    startMdns();
    Serial.printf(
      "Wi-Fi connected: %s or http://%s.local\n",
      configurationUrl().c_str(),
      deviceId_.c_str()
    );
    return;
  }
  startAccessPoint();
}

void SpacePCPortal::maintainWifi() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    wifiDisconnectedSince_ = 0;
    if (wifiConnected_) {
      return;
    }
    wifiConnected_ = true;
    stopAccessPoint();
    startMdns();
    Serial.printf(
      "Wi-Fi reconnected: %s or http://%s.local\n",
      configurationUrl().c_str(),
      deviceId_.c_str()
    );
    return;
  }

  if (wifiConnected_) {
    wifiConnected_ = false;
    wifiDisconnectedSince_ = max<uint32_t>(millis(), 1);
    if (mdnsActive_) {
      MDNS.end();
      mdnsActive_ = false;
    }
    if (mqttClient_.connected()) {
      mqttClient_.disconnect();
    }
    Serial.println("Wi-Fi disconnected.");
  }

  if (
    !wifiSsid_.isEmpty() &&
    wifiDisconnectedSince_ != 0 &&
    millis() - wifiDisconnectedSince_ >= wifiAccessPointFallbackMs
  ) {
    startAccessPoint();
  }

  if (
    wifiSsid_.isEmpty() ||
    millis() - lastWifiAttempt_ < wifiRetryIntervalMs
  ) {
    return;
  }
  lastWifiAttempt_ = millis();
  Serial.println("Attempting Wi-Fi reconnect.");
  WiFi.reconnect();
}

void SpacePCPortal::startAccessPoint() {
  if (accessPointActive_) {
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  accessPointActive_ = WiFi.softAP(
    accessPointName_.c_str(),
    setupAccessPointPassword
  );
  if (!accessPointActive_) {
    return;
  }
  dnsServer_.start(53, "*", WiFi.softAPIP());
  Serial.printf(
    "Setup access point: %s, password: %s, address: %s\n",
    accessPointName_.c_str(),
    setupAccessPointPassword,
    configurationUrl().c_str()
  );
}

void SpacePCPortal::stopAccessPoint() {
  if (!accessPointActive_) {
    return;
  }
  dnsServer_.stop();
  WiFi.softAPdisconnect(false);
  accessPointActive_ = false;
  WiFi.mode(WIFI_STA);
  Serial.println("Setup access point stopped.");
}

void SpacePCPortal::startWebServer() {
  webServer_.on("/", HTTP_GET, [this] {
    webServer_.send(200, "text/html; charset=utf-8", renderPage());
  });
  webServer_.on("/save", HTTP_POST, [this] {
    handleSave();
  });
  webServer_.on("/api/status", HTTP_GET, [this] {
    handleStatus();
  });
  webServer_.on("/api/v1/info", HTTP_GET, [this] {
    handleApiInfo();
  });
  webServer_.on("/api/v1/state", HTTP_GET, [this] {
    handleApiState();
  });
  webServer_.on("/api/v1/display", HTTP_PUT, [this] {
    handleDisplayUpdate();
  });
  webServer_.on("/generate_204", HTTP_ANY, [this] {
    redirectToPortal();
  });
  webServer_.on("/hotspot-detect.html", HTTP_ANY, [this] {
    redirectToPortal();
  });
  webServer_.on("/connecttest.txt", HTTP_ANY, [this] {
    redirectToPortal();
  });
  webServer_.onNotFound([this] {
    redirectToPortal();
  });
  webServer_.begin();
}

bool SpacePCPortal::connectMqtt() {
  if (
    WiFi.status() != WL_CONNECTED ||
    mqttHost_.isEmpty() ||
    mqttClient_.connected()
  ) {
    return mqttClient_.connected();
  }

  mqttClient_.setServer(mqttHost_.c_str(), mqttPort_);
  const bool connected = mqttUsername_.isEmpty()
    ? mqttClient_.connect(
        deviceId_.c_str(),
        availabilityTopic().c_str(),
        1,
        true,
        "offline"
      )
    : mqttClient_.connect(
        deviceId_.c_str(),
        mqttUsername_.c_str(),
        mqttPassword_.c_str(),
        availabilityTopic().c_str(),
        1,
        true,
        "offline"
      );

  if (connected) {
    mqttClient_.publish(availabilityTopic().c_str(), "online", true);
    publishDiscovery();
  }
  return connected;
}

void SpacePCPortal::publishDiscovery() {
  for (size_t index = 0; index < entityCount_; index += 1) {
    const SpacePCHomeAssistantEntity &entity = entities_[index];
    const String topic = discoveryTopic(entity.objectId);
    if (!homeAssistantDiscovery_) {
      mqttClient_.publish(topic.c_str(), "", true);
      continue;
    }

    String payload = "{";
    payload += "\"name\":\"" + jsonEscape(entity.name) + "\",";
    payload +=
      "\"unique_id\":\"" +
      deviceId_ +
      "_" +
      jsonEscape(entity.objectId) +
      "\",";
    payload += "\"state_topic\":\"" + stateTopic() + "\",";
    payload += "\"availability_topic\":\"" + availabilityTopic() + "\",";
    payload +=
      "\"value_template\":\"" +
      jsonEscape(entity.valueTemplate) +
      "\",";
    if (entity.deviceClass && strlen(entity.deviceClass) > 0) {
      payload +=
        "\"device_class\":\"" +
        jsonEscape(entity.deviceClass) +
        "\",";
    }
    if (entity.stateClass && strlen(entity.stateClass) > 0) {
      payload +=
        "\"state_class\":\"" +
        jsonEscape(entity.stateClass) +
        "\",";
    }
    if (entity.unit && strlen(entity.unit) > 0) {
      payload +=
        "\"unit_of_measurement\":\"" +
        jsonEscape(entity.unit) +
        "\",";
    }
    payload += "\"device\":{";
    payload += "\"identifiers\":[\"" + deviceId_ + "\"],";
    payload += "\"name\":\"" + jsonEscape(deviceName_) + "\",";
    payload += "\"manufacturer\":\"SpacePC\",";
    payload +=
      "\"model\":\"" +
      jsonEscape(config_.model ? config_.model : "ESP32 device") +
      "\"";
    payload += "}}";
    mqttClient_.publish(topic.c_str(), payload.c_str(), true);
  }
}

String SpacePCPortal::renderPage() const {
  String page(spacePCPortalPage);
  page.replace(
    "{{PROJECT_NAME}}",
    htmlEscape(config_.displayName ? config_.displayName : "SpacePC device")
  );
  page.replace("{{DEVICE_NAME}}", htmlEscape(deviceName_));
  page.replace("{{INTERVAL}}", String(publishIntervalSeconds_));
  page.replace(
    "{{PROJECT_SETTINGS_TITLE}}",
    htmlEscape(
      config_.projectSettingsTitle
        ? config_.projectSettingsTitle
        : "Project settings"
    )
  );
  page.replace("{{PROJECT_FIELDS}}", renderProjectFields());
  page.replace("{{WIFI_SSID}}", htmlEscape(wifiSsid_));
  page.replace("{{MQTT_HOST}}", htmlEscape(mqttHost_));
  page.replace("{{MQTT_PORT}}", String(mqttPort_));
  page.replace("{{MQTT_USERNAME}}", htmlEscape(mqttUsername_));
  page.replace("{{MQTT_TOPIC}}", htmlEscape(mqttBaseTopic_));
  page.replace(
    "{{HA_CHECKED}}",
    homeAssistantDiscovery_ ? "checked" : ""
  );
  return page;
}

String SpacePCPortal::renderProjectFields() const {
  String html;
  for (size_t index = 0; index < numberFieldCount_; index += 1) {
    const NumberFieldState &field = numberFields_[index];
    html += "<div class=\"field\"><label for=\"" + String(field.definition.key) + "\">";
    html += htmlEscape(field.definition.label);
    html += "</label><input type=\"number\" id=\"" + String(field.definition.key);
    html += "\" name=\"" + String(field.definition.key);
    html += "\" min=\"" + String(field.definition.minimum);
    html += "\" max=\"" + String(field.definition.maximum);
    html += "\" required value=\"" + String(field.value) + "\">";
    if (field.definition.help && strlen(field.definition.help) > 0) {
      html += "<small>" + htmlEscape(field.definition.help) + "</small>";
    }
    html += "</div>";
  }
  for (size_t index = 0; index < textFieldCount_; index += 1) {
    const TextFieldState &field = textFields_[index];
    html += "<div class=\"field\"><label for=\"" + String(field.definition.key) + "\">";
    html += htmlEscape(field.definition.label);
    html += "</label><input type=\"text\" id=\"" + String(field.definition.key);
    html += "\" name=\"" + String(field.definition.key);
    html += "\" maxlength=\"" + String(field.definition.maximumLength);
    html += "\" value=\"" + htmlEscape(field.value) + "\">";
    if (field.definition.help && strlen(field.definition.help) > 0) {
      html += "<small>" + htmlEscape(field.definition.help) + "</small>";
    }
    html += "</div>";
  }
  for (size_t index = 0; index < checkboxFieldCount_; index += 1) {
    const CheckboxFieldState &field = checkboxFields_[index];
    html += "<div class=\"field\"><label class=\"check\"><input type=\"checkbox\" name=\"";
    html += String(field.definition.key) + "\" value=\"1\" ";
    html += field.value ? "checked" : "";
    html += ">" + htmlEscape(field.definition.label) + "</label>";
    if (field.definition.help && strlen(field.definition.help) > 0) {
      html += "<small>" + htmlEscape(field.definition.help) + "</small>";
    }
    html += "</div>";
  }
  if (html.isEmpty()) {
    html = "<small>This project has no additional settings.</small>";
  }
  return html;
}

void SpacePCPortal::handleSave() {
  deviceName_ = webServer_.arg("deviceName");
  wifiSsid_ = webServer_.arg("wifiSsid");
  mqttHost_ = webServer_.arg("mqttHost");
  mqttPort_ = static_cast<uint16_t>(webServer_.arg("mqttPort").toInt());
  mqttUsername_ = webServer_.arg("mqttUsername");
  mqttBaseTopic_ = normalizedTopic(webServer_.arg("mqttBaseTopic"));
  homeAssistantDiscovery_ = webServer_.hasArg("homeAssistantDiscovery");
  publishIntervalSeconds_ = webServer_.arg("interval").toInt();

  const String wifiPassword = webServer_.arg("wifiPassword");
  const String mqttPassword = webServer_.arg("mqttPassword");
  if (webServer_.hasArg("clearWifiPassword")) {
    wifiPassword_ = "";
  } else if (!wifiPassword.isEmpty()) {
    wifiPassword_ = wifiPassword;
  }
  if (webServer_.hasArg("clearMqttPassword")) {
    mqttPassword_ = "";
  } else if (!mqttPassword.isEmpty()) {
    mqttPassword_ = mqttPassword;
  }

  deviceName_.trim();
  wifiSsid_.trim();
  mqttHost_.trim();
  mqttUsername_.trim();

  bool valid =
    !deviceName_.isEmpty() &&
    !wifiSsid_.isEmpty() &&
    mqttPort_ > 0 &&
    validPublishTopic(mqttBaseTopic_) &&
    publishIntervalSeconds_ >= 5 &&
    publishIntervalSeconds_ <= 86400;

  for (size_t index = 0; index < numberFieldCount_; index += 1) {
    NumberFieldState &field = numberFields_[index];
    field.value = webServer_.arg(field.definition.key).toInt();
    valid &= field.value >= field.definition.minimum;
    valid &= field.value <= field.definition.maximum;
  }
  for (size_t index = 0; index < textFieldCount_; index += 1) {
    TextFieldState &field = textFields_[index];
    field.value = webServer_.arg(field.definition.key);
    field.value.trim();
    valid &= field.value.length() <= field.definition.maximumLength;
  }
  for (size_t index = 0; index < checkboxFieldCount_; index += 1) {
    CheckboxFieldState &field = checkboxFields_[index];
    field.value = webServer_.hasArg(field.definition.key);
  }

  if (!valid) {
    webServer_.send(
      400,
      "text/plain",
      "Invalid settings. Check all required fields and value ranges."
    );
    return;
  }
  if (!saveSettings()) {
    webServer_.send(500, "text/plain", "Could not store settings.");
    return;
  }

  webServer_.send(
    200,
    "text/html",
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<p>Settings saved. The device is restarting…</p>"
  );
  delay(500);
  ESP.restart();
}

void SpacePCPortal::handleStatus() {
  String payload = "{";
  payload += "\"project\":\"" + jsonEscape(projectStatus_) + "\",";
  payload += "\"wifi\":\"";
  payload += WiFi.status() == WL_CONNECTED ? "connected" : "setup access point";
  payload += "\",\"mqtt\":\"";
  payload += mqttClient_.connected() ? "connected" : "disconnected";
  payload += "\",\"ip\":\"";
  payload += WiFi.status() == WL_CONNECTED
    ? WiFi.localIP().toString()
    : WiFi.softAPIP().toString();
  payload += "\"}";
  webServer_.send(200, "application/json", payload);
}

void SpacePCPortal::handleApiInfo() {
  String payload;
  payload.reserve(768 + entityCount_ * 192);
  payload = "{";
  payload += "\"api_version\":1,";
  payload += "\"device_id\":\"" + jsonEscape(deviceId_) + "\",";
  payload += "\"name\":\"" + jsonEscape(deviceName_) + "\",";
  payload += "\"manufacturer\":\"SpacePC\",";
  payload +=
    "\"model\":\"" +
    jsonEscape(config_.model ? config_.model : "ESP32 device") +
    "\",";
  payload +=
    "\"project_id\":\"" +
    jsonEscape(config_.projectId ? config_.projectId : "spacepc-device") +
    "\",";
  payload += "\"firmware\":{";
  payload +=
    "\"version\":\"" +
    jsonEscape(
      config_.firmwareVersion ? config_.firmwareVersion : "0.0.0-dev"
    ) +
    "\"";
  if (config_.firmwareBuildDate && strlen(config_.firmwareBuildDate) > 0) {
    payload +=
      ",\"build_date\":\"" +
      jsonEscape(config_.firmwareBuildDate) +
      "\"";
  }
  if (config_.sourceCommit && strlen(config_.sourceCommit) > 0) {
    payload +=
      ",\"source_commit\":\"" +
      jsonEscape(config_.sourceCommit) +
      "\"";
  }
  payload += "},\"auth_required\":false,\"entities\":[";

  for (size_t index = 0; index < entityCount_; index += 1) {
    if (index > 0) {
      payload += ",";
    }
    const SpacePCHomeAssistantEntity &entity = entities_[index];
    payload += "{";
    payload += "\"id\":\"" + jsonEscape(entity.objectId) + "\",";
    payload += "\"name\":\"" + jsonEscape(entity.name) + "\",";
    payload +=
      "\"platform\":\"" +
      jsonEscape(entityPlatform(entity)) +
      "\"";
    if (entity.deviceClass && strlen(entity.deviceClass) > 0) {
      payload +=
        ",\"device_class\":\"" +
        jsonEscape(entity.deviceClass) +
        "\"";
    }
    if (entity.stateClass && strlen(entity.stateClass) > 0) {
      payload +=
        ",\"state_class\":\"" +
        jsonEscape(entity.stateClass) +
        "\"";
    }
    if (entity.unit && strlen(entity.unit) > 0) {
      payload +=
        ",\"unit\":\"" +
        jsonEscape(entity.unit) +
        "\"";
    }
    payload += "}";
  }
  payload += "]";
  if (!displayCapabilitiesJson_.isEmpty()) {
    payload += ",\"display\":" + displayCapabilitiesJson_;
  }
  payload += "}";

  webServer_.sendHeader("Cache-Control", "no-store");
  webServer_.send(200, "application/json", payload);
}

void SpacePCPortal::handleApiState() {
  String payload;
  payload.reserve(384 + entityCount_ * 96);
  payload = "{\"entities\":{";
  for (size_t index = 0; index < entityCount_; index += 1) {
    if (index > 0) {
      payload += ",";
    }
    payload += "\"" + jsonEscape(entities_[index].objectId) + "\":{";
    payload += "\"value\":" + entityValues_[index] + ",";
    payload += "\"available\":";
    payload += entityAvailable_[index] ? "true" : "false";
    payload += "}";
  }
  payload += "},\"diagnostics\":{";
  payload += "\"uptime_seconds\":" + String(millis() / 1000UL) + ",";
  payload += "\"wifi_rssi_dbm\":";
  payload += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : "null";
  payload += ",\"free_heap_bytes\":" + String(ESP.getFreeHeap());
  payload += "}}";

  webServer_.sendHeader("Cache-Control", "no-store");
  webServer_.send(200, "application/json", payload);
}

void SpacePCPortal::handleDisplayUpdate() {
  if (!displayUpdateHandler_) {
    webServer_.send(404, "application/json", "{\"error\":\"not_supported\"}");
    return;
  }
  const String body = webServer_.arg("plain");
  if (body.isEmpty() || body.length() > 16384) {
    webServer_.send(400, "application/json", "{\"error\":\"invalid_payload\"}");
    return;
  }
  String errorMessage;
  if (!displayUpdateHandler_(body, errorMessage)) {
    errorMessage.replace("\\", "\\\\");
    errorMessage.replace("\"", "\\\"");
    webServer_.send(
      422,
      "application/json",
      "{\"error\":\"" + errorMessage + "\"}"
    );
    return;
  }
  webServer_.send(202, "application/json", "{\"accepted\":true}");
}

void SpacePCPortal::redirectToPortal() {
  webServer_.sendHeader("Location", "/", true);
  webServer_.send(302, "text/plain", "");
}

void SpacePCPortal::startMdns() {
  if (mdnsActive_) {
    MDNS.end();
    mdnsActive_ = false;
  }
  if (!MDNS.begin(deviceId_.c_str())) {
    Serial.println("Could not start mDNS.");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("spacepc", "tcp", 80);
  MDNS.addServiceTxt("spacepc", "tcp", "id", deviceId_);
  MDNS.addServiceTxt("spacepc", "tcp", "api", "1");
  MDNS.addServiceTxt(
    "spacepc",
    "tcp",
    "project",
    config_.projectId ? config_.projectId : "spacepc-device"
  );
  MDNS.addServiceTxt("spacepc", "tcp", "path", localApiPath);
  mdnsActive_ = true;
}

String SpacePCPortal::stateTopic() const {
  return normalizedTopic(mqttBaseTopic_) + "/state";
}

String SpacePCPortal::availabilityTopic() const {
  return normalizedTopic(mqttBaseTopic_) + "/availability";
}

String SpacePCPortal::discoveryTopic(const char *objectId) const {
  return
    "homeassistant/sensor/" +
    deviceId_ +
    "/" +
    String(objectId) +
    "/config";
}

const char *SpacePCPortal::entityPlatform(
  const SpacePCHomeAssistantEntity &entity
) const {
  return entity.platform && strlen(entity.platform) > 0
    ? entity.platform
    : "sensor";
}

int SpacePCPortal::entityIndex(const char *objectId) const {
  if (!objectId) {
    return -1;
  }
  for (size_t index = 0; index < entityCount_; index += 1) {
    if (strcmp(entities_[index].objectId, objectId) == 0) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

bool SpacePCPortal::fieldKeyExists(const char *key) const {
  for (size_t index = 0; index < numberFieldCount_; index += 1) {
    if (strcmp(numberFields_[index].definition.key, key) == 0) {
      return true;
    }
  }
  for (size_t index = 0; index < textFieldCount_; index += 1) {
    if (strcmp(textFields_[index].definition.key, key) == 0) {
      return true;
    }
  }
  for (size_t index = 0; index < checkboxFieldCount_; index += 1) {
    if (strcmp(checkboxFields_[index].definition.key, key) == 0) {
      return true;
    }
  }
  return false;
}
