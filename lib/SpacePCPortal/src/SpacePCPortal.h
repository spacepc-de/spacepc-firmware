#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>

struct SpacePCPortalConfig {
  const char *projectId;
  const char *displayName;
  const char *model;
};

struct SpacePCNumberField {
  const char *key;
  const char *label;
  const char *help;
  int defaultValue;
  int minimum;
  int maximum;
};

struct SpacePCTextField {
  const char *key;
  const char *label;
  const char *help;
  const char *defaultValue;
  size_t maximumLength;
};

struct SpacePCCheckboxField {
  const char *key;
  const char *label;
  const char *help;
  bool defaultValue;
};

struct SpacePCHomeAssistantEntity {
  const char *objectId;
  const char *name;
  const char *deviceClass;
  const char *stateClass;
  const char *unit;
  const char *valueTemplate;
};

class SpacePCPortal {
 public:
  SpacePCPortal();

  bool addNumberField(const SpacePCNumberField &field);
  bool addTextField(const SpacePCTextField &field);
  bool addCheckboxField(const SpacePCCheckboxField &field);
  bool addHomeAssistantEntity(const SpacePCHomeAssistantEntity &entity);

  void begin(const SpacePCPortalConfig &config);
  void loop();

  int numberValue(const char *key) const;
  String textValue(const char *key) const;
  bool checkboxValue(const char *key) const;

  bool mqttConnected();
  bool publishDue();
  bool publishState(const String &jsonPayload);
  void setProjectStatus(const String &status);

  String deviceId() const;
  String configurationUrl() const;

 private:
  static const size_t maxFields = 12;
  static const size_t maxEntities = 12;

  struct NumberFieldState {
    SpacePCNumberField definition;
    int value;
  };

  struct TextFieldState {
    SpacePCTextField definition;
    String value;
  };

  struct CheckboxFieldState {
    SpacePCCheckboxField definition;
    bool value;
  };

  SpacePCPortalConfig config_;
  WiFiClient networkClient_;
  PubSubClient mqttClient_;
  WebServer webServer_;
  DNSServer dnsServer_;

  NumberFieldState numberFields_[maxFields];
  TextFieldState textFields_[maxFields];
  CheckboxFieldState checkboxFields_[maxFields];
  SpacePCHomeAssistantEntity entities_[maxEntities];
  size_t numberFieldCount_;
  size_t textFieldCount_;
  size_t checkboxFieldCount_;
  size_t entityCount_;

  String deviceName_;
  String wifiSsid_;
  String wifiPassword_;
  String mqttHost_;
  uint16_t mqttPort_;
  String mqttUsername_;
  String mqttPassword_;
  String mqttBaseTopic_;
  bool homeAssistantDiscovery_;
  uint32_t publishIntervalSeconds_;

  String deviceId_;
  String settingsNamespace_;
  String accessPointName_;
  String projectStatus_;
  bool accessPointActive_;
  uint32_t lastMqttAttempt_;
  uint32_t lastPublish_;

  void loadSettings();
  bool saveSettings();
  void connectWifi();
  void startAccessPoint();
  void startWebServer();
  bool connectMqtt();
  void publishDiscovery();

  String renderPage() const;
  String renderProjectFields() const;
  void handleSave();
  void handleStatus();
  void redirectToPortal();

  String stateTopic() const;
  String availabilityTopic() const;
  String discoveryTopic(const char *objectId) const;
  bool fieldKeyExists(const char *key) const;
};
