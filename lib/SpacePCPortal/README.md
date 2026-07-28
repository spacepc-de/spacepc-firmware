# SpacePCPortal

Shared configuration and connectivity layer for SpacePC ESP32 firmware.

Every project using this library receives the same:

- protected Wi-Fi setup access point and captive portal;
- automatic Wi-Fi reconnection and mDNS re-registration;
- responsive local setup page;
- persistent device, Wi-Fi and MQTT settings;
- MQTT state and availability topics;
- transmission interval;
- Home Assistant MQTT Discovery switch;
- native SpacePC API v1 and mDNS discovery for Home Assistant;
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

void loop() {
  portal.loop();
  if (portal.publishDue()) {
    const float temperature = readTemperature();
    portal.setEntityState("temperature", String(temperature, 2));
    portal.publishState("{\"temperature\":" + String(temperature, 2) + "}");
  }
}
```

Supported project field types are number, text and checkbox. Field keys must be
unique, contain at most 12 alphanumeric/`-`/`_` characters and are stored in the
ESP32 Preferences namespace. The project ID becomes that namespace and should
therefore contain at most 15 characters; longer IDs fall back to `spacepc`.

The project is responsible for reading hardware and updating registered entity
state with `setEntityState()`. Values are JSON literals: pass numbers and
booleans directly; strings must be JSON-escaped and quoted. Mark failed or
missing hardware with `setEntityUnavailable()`.

`publishState()` remains the optional MQTT output. Sensor reads and the native
Home Assistant API do not depend on an MQTT connection.

The portal retries a lost Wi-Fi connection every ten seconds. After reconnect,
it stops any fallback setup access point, advertises the SpacePC mDNS service
again and then allows MQTT to reconnect independently.

When connected to Wi-Fi, every device advertises `_spacepc._tcp.local.` with
its stable device ID, API version and project ID. The native API endpoints are:

- `GET /api/v1/info` for identity, firmware and entity definitions;
- `GET /api/v1/state` for current entity values and diagnostics.

The API follows the contract documented in
[`home-assistant-spacepc`](https://github.com/spacepc-de/home-assistant-spacepc/blob/main/docs/api-v1.md).

MQTT remains available independently of Home Assistant. It is currently
unencrypted and intended for a trusted local network. The native API currently
does not require authentication and must also be used only on a trusted local
network until device tokens are implemented.
