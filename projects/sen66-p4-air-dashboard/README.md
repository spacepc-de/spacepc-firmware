# SEN66 P4 Air Dashboard

Landscape LVGL dashboard for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 and
Sensirion SEN66. It displays CO2, PM2.5, PM10, VOC, NOx, temperature and
humidity, plus a touch-selectable 60-sample CO2/PM2.5 trend.

## Wiring

The board display and touch remain onboard. Connect the SEN66 to the exposed
I2C header:

| SEN66 | Waveshare board |
|---|---|
| VDD | 3V3 |
| GND | GND |
| SDA | GPIO7 / SDA |
| SCL | GPIO8 / SCL |

The touch controller and sensor share this bus because they have
different addresses (GT911 and SEN66 `0x6B`).

## Build and flash

ESP-IDF 5.4 or newer is required.

```sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.wchusbserial5B901592401 flash monitor
```

The SEN66 needs several seconds after boot before its first valid measurement.
