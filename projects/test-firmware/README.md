# Test Firmware

Universal diagnostic firmware for the Wi-Fi-capable ESP32, ESP32-S2, ESP32-S3,
and ESP32-C3 families. On startup it creates the intentionally open
`SpacePC-Test-XXXXXX` access point without a Wi-Fi password. The captive portal
opens automatically on most devices and is also available at
`http://192.168.4.1`.

The live dashboard displays:

- chip model, revision, cores, frequency, features, and internal temperature
- reset reason, uptime, and eFuse ID
- heap, PSRAM, sketch, flash, and NVS partition information
- Arduino Core, ESP-IDF, and build versions
- access point address, MAC, channel, and connected clients

The tools section also provides:

- an I²C scanner with selectable SDA and SCL GPIOs
- a PSRAM memory test that verifies and zeros allocatable free PSRAM
- a controlled restart
- complete NVS erasure, including old Wi-Fi credentials
- an explanation of where data is stored in flash, NVS, internal RAM, and PSRAM

The same diagnostic values are available as JSON at `/api/status` and are
printed to the serial console every five seconds at 115200 baud. The dashboard
can also download the current data set as a JSON file.

This test firmware does not use `Preferences` or write its own settings to NVS.
The Arduino Wi-Fi stack or previously installed firmware may leave credentials
and settings in NVS that survive a normal reflash. After confirmation, the
tools section can erase the entire NVS partition. Firmware, bootloader, NVS,
and the partition table reside in internal flash. Heap and PSRAM are volatile.
The PSRAM test only overwrites memory it successfully allocated; memory used by
the running firmware remains untouched.

> The test Wi-Fi network is intentionally unencrypted. The firmware does not
> provide Internet access and should not be operated permanently or in
> sensitive environments.

## Build

```sh
cd projects/test-firmware
pio run
```

Build a single target with a command such as
`pio run -e esp32-c3-devkit`. The generated images target the documented
generic Espressif development boards. Boards in the same chip family with a
different flash size or memory connection may require a custom PlatformIO board
configuration. ESP32-H2 and ESP32-P4 are excluded because they do not provide
the integrated 2.4 GHz Wi-Fi required by the captive portal. ESP32-C6 is not
yet supported by the pinned Arduino toolchain used by this repository.
