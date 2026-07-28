# ESP32 DS18B20 temperature sensor

This firmware reads up to two DS18B20 temperature sensors on separate GPIOs and
exposes each configured sensor through the local SpacePC API. It includes a
browser-based configuration page,
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
| DATA sensor 1 | Configured sensor 1 GPIO |
| DATA sensor 2 | Configured sensor 2 GPIO |

Install a **4.7 kΩ pull-up resistor between DATA and 3.3 V**. The firmware does
not document or enable parasite-power mode. It also enables the ESP32's weak
internal pull-up for compatibility with short three-wire sensor assemblies,
but this does not replace the external 4.7 kΩ resistor for a reliable final
installation. This is a low-voltage project.

The first data pin defaults to GPIO 4. The second sensor is disabled by default;
set its GPIO to `-1` to keep it disabled. Each active GPIO needs its own 4.7 kΩ
pull-up resistor. Pin availability and boot-strapping behavior differ between
ESP32 families and individual boards. Select safe, distinct pins for the exact
board in the web interface.

## First-time setup

1. Flash the correct build for the ESP32 family.
2. Join the Wi-Fi network `SpacePC-Temp-xxxxxx`.
3. Use password `spacepcsetup`.
4. Open `http://192.168.4.1` if the captive portal does not appear.
5. Enter Wi-Fi, sensor GPIOs, sensor names and optional MQTT settings, then save.

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
- `GET /api/v1/state` for each configured temperature, its individual
  availability and device diagnostics.

The first sensor keeps the stable `temperature` entity identifier used by
firmware 0.1.x. An enabled second sensor uses `temperature_2`. Their configured
names are exposed to Home Assistant. If one sensor is disconnected, only its
entity becomes unavailable. The firmware rescans that OneWire bus after failed
readings and restores the entity automatically after the sensor is reconnected.
Wi-Fi and mDNS also recover without rebooting the ESP32.

## MQTT

With base topic `spacepc/temperature`, the firmware publishes retained messages:

```text
spacepc/temperature/state
spacepc/temperature/availability
```

State payload:

```json
{"temperature":21.50,"temperature_2":19.75}
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

- up to two DS18B20 sensors on separate GPIOs;
- Celsius output only;
- no MQTT TLS;
- no remote firmware update;
- GPIO selection is validated by numeric range, but electrical suitability
  must be checked against the exact board schematic.
