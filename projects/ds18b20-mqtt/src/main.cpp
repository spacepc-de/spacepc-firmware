#include <Arduino.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <SpacePCPortal.h>

namespace {
constexpr uint32_t failedSensorRetryMs = 5000;

SpacePCPortal portal;
OneWire *oneWire = nullptr;
DallasTemperature *temperatureSensors = nullptr;
uint32_t lastSensorAttempt = 0;

const SpacePCPortalConfig projectConfig = {
  "ds18b20-mqtt",
  "DS18B20 temperature sensor",
  "ESP32 DS18B20 Sensor",
  "0.1.0-dev",
  nullptr,
  nullptr
};

const SpacePCNumberField sensorPinField = {
  "sensorPin",
  "DS18B20 GPIO",
  "Use a GPIO available on the exact board. Add a 4.7 kΩ pull-up between DATA and 3.3 V.",
  4,
  0,
  48
};

const SpacePCHomeAssistantEntity temperatureEntity = {
  "temperature",
  "Temperature",
  "temperature",
  "measurement",
  "°C",
  "{{ value_json.temperature }}"
};

void initializeSensor() {
  const int sensorPin = portal.numberValue("sensorPin");
  oneWire = new OneWire(sensorPin);
  pinMode(sensorPin, INPUT_PULLUP);
  delay(10);
  const bool presenceDetected = oneWire->reset();
  Serial.printf(
    "DS18B20 GPIO: %d, bus level: %s, presence pulse: %s\n",
    sensorPin,
    digitalRead(sensorPin) == HIGH ? "high" : "low",
    presenceDetected ? "detected" : "missing"
  );
  temperatureSensors = new DallasTemperature(oneWire);
  temperatureSensors->begin();
  Serial.printf(
    "DS18B20 devices found: %d\n",
    temperatureSensors->getDeviceCount()
  );
  portal.setProjectStatus(
    temperatureSensors->getDeviceCount() > 0 ? "ready" : "sensor not found"
  );
}

void readAndPublishTemperature() {
  if (!temperatureSensors) {
    return;
  }
  lastSensorAttempt = millis();
  temperatureSensors->requestTemperatures();
  const float temperature = temperatureSensors->getTempCByIndex(0);
  const bool validReading =
    temperature != DEVICE_DISCONNECTED_C &&
    temperature >= -55.0F &&
    temperature <= 125.0F;
  if (!validReading) {
    portal.setProjectStatus("sensor not found");
    portal.setEntityUnavailable("temperature");
    portal.publishState("{\"temperature\":null}");
    temperatureSensors->begin();
    Serial.println("DS18B20 reading failed.");
    return;
  }

  portal.setProjectStatus("ready");
  portal.setEntityState("temperature", String(temperature, 2));
  char payload[48];
  snprintf(payload, sizeof(payload), "{\"temperature\":%.2f}", temperature);
  if (portal.publishState(payload)) {
    Serial.printf("Published temperature: %.2f °C\n", temperature);
  }
}
}

void setup() {
  Serial.begin(115200);
  delay(250);

  if (
    !portal.addNumberField(sensorPinField) ||
    !portal.addHomeAssistantEntity(temperatureEntity)
  ) {
    Serial.println("Could not register project configuration.");
  }

  portal.begin(projectConfig);
  initializeSensor();
  Serial.printf("Configuration: %s\n", portal.configurationUrl().c_str());
}

void loop() {
  portal.loop();
  if (
    portal.publishDue() &&
    (lastSensorAttempt == 0 || millis() - lastSensorAttempt >= failedSensorRetryMs)
  ) {
    readAndPublishTemperature();
  }
}
