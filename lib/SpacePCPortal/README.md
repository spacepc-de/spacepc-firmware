# SpacePCPortal

Shared configuration and connectivity layer for SpacePC ESP32 firmware.

Every project using this library receives the same:

- protected Wi-Fi setup access point and captive portal;
- responsive local setup page;
- persistent device, Wi-Fi and MQTT settings;
- MQTT state and availability topics;
- transmission interval;
- Home Assistant MQTT Discovery switch;
- status API and project-specific settings section.

Projects register only their additional fields and Home Assistant entities:

```cpp
SpacePCPortal portal;

const SpacePCNumberField gpioField = {
  "sensorPin",
  "Sensor GPIO",
  "Choose a pin supported by the exact board.",
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

void setup() {
  portal.addNumberField(gpioField);
  portal.addHomeAssistantEntity(temperatureEntity);
  portal.begin({"project-id", "Project name", "Hardware model"});
}
```

Supported project field types are number, text and checkbox. Field keys must be
unique, contain at most 12 alphanumeric/`-`/`_` characters and are stored in the
ESP32 Preferences namespace. The project ID becomes that namespace and should
therefore contain at most 15 characters; longer IDs fall back to `spacepc`.

The project is responsible for reading hardware and publishing one JSON state
object with `publishState()`. Registered Home Assistant entities select values
from that object through their templates.

MQTT is intentionally always part of the portal. It is currently unencrypted
and intended for a trusted local network. Projects must not claim Home
Assistant support until their registered entities have been tested with a real
broker and Home Assistant installation.
