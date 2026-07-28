# ESP32 DS18B20 temperature sensor

This firmware reads one DS18B20 temperature sensor and exposes the result
through the local SpacePC API. It includes a browser-based configuration page,
automatic discovery by the native SpacePC Home Assistant integration and
optional MQTT publishing.

The setup interface, Wi-Fi provisioning, local API, mDNS discovery, MQTT
connection and MQTT Discovery behavior come from the shared `SpacePCPortal`
library. This project adds only the DS18B20 GPIO field, temperature reading and
temperature entity.

## Supported build targets

The project is compiled for these generic PlatformIO development-board targets:

- classic ESP32 DevKit (`esp32dev`);
- ESP32-S2 Saola-1;
- ESP32-S3 DevKitC-1;
- ESP32-C3 DevKitM-1.

These targets verify compilation, not every third-party board sold under a
similar name. Confirm the pinout, USB implementation and flash layout of the
exact board before installing firmware.

## Wiring

DS18B20 in externally powered three-wire mode:

| DS18B20 | ESP32 |
| --- | --- |
| GND | GND |
| VDD | 3.3 V |
| DATA | Configured GPIO |

Install a **4.7 kΩ pull-up resistor between DATA and 3.3 V**. The firmware does
not document or enable parasite-power mode. This is a low-voltage project.

The default data pin is GPIO 4. Pin availability and boot-strapping behavior
differ between ESP32 families and individual boards. Select a safe pin for the
exact board in the web interface.

## First-time setup

1. Flash the correct build for the ESP32 family.
2. Join the Wi-Fi network `SpacePC-Temp-xxxxxx`.
3. Use password `spacepcsetup`.
4. Open `http://192.168.4.1` if the captive portal does not appear.
5. Enter Wi-Fi, GPIO and MQTT settings, then save.

After joining Wi-Fi, the configuration page remains available at the IP shown
in the serial log and usually at `http://spacepc-xxxxxxxxxxxx.local`.

Passwords are never returned into the HTML form. Leaving a password field empty
keeps the stored value; use the adjacent checkbox to clear a stored password.

MQTT is optional for native Home Assistant use. Leave the broker field empty
when no MQTT connection is wanted.

## Native Home Assistant integration

After joining Wi-Fi, the device advertises `_spacepc._tcp.local.` and is
discovered by the
[`SpacePC integration`](https://github.com/spacepc-de/home-assistant-spacepc).
No MQTT broker is required for this path.

The device exposes:

- `GET /api/v1/info` for device, firmware and entity metadata;
- `GET /api/v1/state` for temperature, availability and diagnostics.

## MQTT

With base topic `spacepc/temperature`, the firmware publishes retained messages:

```text
spacepc/temperature/state
spacepc/temperature/availability
```

State payload:

```json
{"temperature":21.50}
```

When Home Assistant discovery is enabled, the retained sensor configuration is
published below:

```text
homeassistant/sensor/<device-id>/temperature/config
```

The device uses local, unencrypted MQTT on the configured TCP port. Run it only
on a trusted network or isolate it in an IoT VLAN. TLS is not currently
implemented.

## Local build

```sh
cd projects/ds18b20-mqtt
pio run
```

Build only one family:

```sh
pio run -e esp32-s3-devkit
```

## Current limitations

- one DS18B20 sensor per device;
- Celsius output only;
- no MQTT TLS;
- no remote firmware update;
- GPIO selection is validated by numeric range, but electrical suitability
  must be checked against the exact board schematic.
