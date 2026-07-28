#include <Arduino.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <SpacePCPortal.h>

namespace {
constexpr size_t sensorCount = 2;
constexpr uint32_t failedSensorRetryMs = 5000;

struct SensorChannel {
  const char *entityId;
  const char *pinKey;
  const char *nameKey;
  OneWire *oneWire;
  DallasTemperature *temperatureSensors;
  bool enabled;
};

SpacePCPortal portal;
uint32_t lastSensorAttempt = 0;
char sensorNames[sensorCount][49] = {};

SensorChannel sensors[sensorCount] = {
  {"temperature", "sensorPin", "sensorName1", nullptr, nullptr, false},
  {"temperature_2", "sensorPin2", "sensorName2", nullptr, nullptr, false}
};

SpacePCHomeAssistantEntity temperatureEntities[sensorCount] = {
  {
    "temperature",
    sensorNames[0],
    "temperature",
    "measurement",
    "°C",
    "{{ value_json.temperature }}"
  },
  {
    "temperature_2",
    sensorNames[1],
    "temperature",
    "measurement",
    "°C",
    "{{ value_json.temperature_2 }}"
  }
};

const SpacePCPortalConfig projectConfig = {
  "ds18b20-mqtt",
  "DS18B20 temperature sensors",
  "ESP32 DS18B20 Sensor",
  "0.2.0",
  "2026-07-28",
  nullptr,
  "Sensors"
};

const SpacePCNumberField sensorPinFields[sensorCount] = {
  {
    "sensorPin",
    "Sensor 1 GPIO",
    "GPIO connected to the first DS18B20 data wire.",
    4,
    0,
    48
  },
  {
    "sensorPin2",
    "Sensor 2 GPIO",
    "Use -1 when no second sensor is connected.",
    -1,
    -1,
    48
  }
};

const SpacePCTextField sensorNameFields[sensorCount] = {
  {
    "sensorName1",
    "Sensor 1 name",
    "Shown as the entity name in Home Assistant.",
    "Temperature 1",
    48
  },
  {
    "sensorName2",
    "Sensor 2 name",
    "For example: Outside temperature or Heating flow.",
    "Temperature 2",
    48
  }
};

bool validTemperature(float temperature) {
  return
    temperature != DEVICE_DISCONNECTED_C &&
    temperature >= -55.0F &&
    temperature <= 125.0F;
}

void initializeSensor(size_t index) {
  SensorChannel &sensor = sensors[index];
  const int sensorPin = portal.numberValue(sensor.pinKey);
  sensor.enabled = sensorPin >= 0;
  if (!sensor.enabled) {
    return;
  }

  sensor.oneWire = new OneWire(sensorPin);
  pinMode(sensorPin, INPUT_PULLUP);
  delay(10);
  const bool presenceDetected = sensor.oneWire->reset();
  Serial.printf(
    "DS18B20 %u GPIO: %d, bus level: %s, presence pulse: %s\n",
    static_cast<unsigned>(index + 1),
    sensorPin,
    digitalRead(sensorPin) == HIGH ? "high" : "low",
    presenceDetected ? "detected" : "missing"
  );
  sensor.temperatureSensors = new DallasTemperature(sensor.oneWire);
  sensor.temperatureSensors->begin();
  Serial.printf(
    "DS18B20 %u devices found: %d\n",
    static_cast<unsigned>(index + 1),
    sensor.temperatureSensors->getDeviceCount()
  );
}

void registerSensors() {
  for (size_t index = 0; index < sensorCount; index += 1) {
    String configuredName = portal.textValue(sensors[index].nameKey);
    configuredName.trim();
    if (configuredName.isEmpty()) {
      configuredName = index == 0 ? "Temperature 1" : "Temperature 2";
    }
    configuredName.toCharArray(sensorNames[index], sizeof(sensorNames[index]));
    if (
      portal.numberValue(sensors[index].pinKey) >= 0 &&
      !portal.addHomeAssistantEntity(temperatureEntities[index])
    ) {
      Serial.printf(
        "Could not register temperature entity %u.\n",
        static_cast<unsigned>(index + 1)
      );
    }
    initializeSensor(index);
  }
}

void readAndPublishTemperatures() {
  lastSensorAttempt = millis();
  String payload = "{";
  bool firstValue = true;
  bool allAvailable = true;

  for (size_t index = 0; index < sensorCount; index += 1) {
    SensorChannel &sensor = sensors[index];
    if (!sensor.enabled || !sensor.temperatureSensors) {
      continue;
    }

    sensor.temperatureSensors->requestTemperatures();
    const float temperature =
      sensor.temperatureSensors->getTempCByIndex(0);
    const bool available = validTemperature(temperature);
    allAvailable &= available;

    if (!firstValue) {
      payload += ",";
    }
    firstValue = false;
    payload += "\"" + String(sensor.entityId) + "\":";

    if (!available) {
      payload += "null";
      portal.setEntityUnavailable(sensor.entityId);
      sensor.temperatureSensors->begin();
      Serial.printf(
        "DS18B20 %u reading failed; rescanning bus.\n",
        static_cast<unsigned>(index + 1)
      );
      continue;
    }

    payload += String(temperature, 2);
    portal.setEntityState(sensor.entityId, String(temperature, 2));
    Serial.printf(
      "%s: %.2f °C\n",
      sensorNames[index],
      temperature
    );
  }

  payload += "}";
  portal.setProjectStatus(allAvailable ? "ready" : "sensor not found");
  portal.publishState(payload);
}
}

void setup() {
  Serial.begin(115200);
  delay(250);

  bool fieldsRegistered = true;
  for (size_t index = 0; index < sensorCount; index += 1) {
    fieldsRegistered &=
      portal.addNumberField(sensorPinFields[index]) &&
      portal.addTextField(sensorNameFields[index]);
  }
  if (!fieldsRegistered) {
    Serial.println("Could not register sensor configuration.");
  }

  portal.begin(projectConfig);
  registerSensors();
  Serial.printf("Configuration: %s\n", portal.configurationUrl().c_str());
}

void loop() {
  portal.loop();
  if (
    portal.publishDue() &&
    (lastSensorAttempt == 0 || millis() - lastSensorAttempt >= failedSensorRetryMs)
  ) {
    readAndPublishTemperatures();
  }
}
