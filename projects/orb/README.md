# Orb

Orb turns the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 into a fluid live window onto the sky.

## Current prototype

- **Sky:** nearby aircraft with continuously moving positions
- **Fullscreen dark map:** real OpenStreetMap streets and towns with high-contrast yellow aircraft overlays; only live state and local time remain at the top
- **Touch navigation:** drag the map, zoom with `+` / `-`, and return to the observer with Home
- **Flight detail:** callsign, aircraft type, registration, altitude, speed, range, bearing, origin, destination and estimated duration
- **Orbit:** animated globe populated from current CelesTrak orbital elements
- **ISS:** live ground position and recent trail on a real dark OpenStreetMap map, plus altitude, speed and observer distance
- **Settings:** swipe down for WiFi, map-centre city and aviation/metric/imperial units
- **Location time:** NTP time is displayed with the automatically resolved time zone and current daylight-saving offset of the observer
- **Offline demo:** every view remains interactive before WiFi is configured

Swipe horizontally to move between Sky, Orbit and ISS. Swipe down to open Settings and up to close it. Orb deliberately uses the display in its 480 x 800 portrait orientation.

The interactive Sky and ISS maps require a microSD card. Orb downloads only the 15 tiles needed by each viewport and keeps them in `/sdcard/ORB/MAPS`, so returning to an area does not download it again. The display converts cached tiles locally into its dark style. Aircraft coverage follows the current map center and zoom automatically; there is no separate range setting. The ISS map follows the station automatically and reprojects its recent trail whenever the viewport moves. Live aircraft and ISS telemetry continue to work if the card is missing.

## Live data

Orb currently uses services that require no account or API key:

| View | Provider | Refresh |
| --- | --- | --- |
| Nearby aircraft | [ADSB.lol](https://api.adsb.lol/) | 15 seconds |
| Flight routes | [ADSBDB](https://www.adsbdb.com/) + position-aware [ADSB.lol](https://api.adsb.lol/docs) fallback | on aircraft selection |
| City search | [Nominatim](https://nominatim.org/release-docs/latest/api/Search/) | on explicit Save only |
| Observer time zone | [TimeAPI.io](https://timeapi.io/) | after location changes and every 6 hours |
| ISS | [Where the ISS at?](https://wheretheiss.at/w/developer) | 10 seconds |
| Visual satellites | [CelesTrak GP data](https://celestrak.org/NORAD/documentation/gp-data-formats.php) | 6 hours |

Aircraft positions are extrapolated between network updates so markers move smoothly. Satellite positions are calculated locally from downloaded mean orbital elements. This first implementation uses a lightweight two-body visual propagation; pass alerts will move to full SGP4 before release.

See [docs/DATA_AND_MAPS.md](docs/DATA_AND_MAPS.md) for provider decisions, limits and the map roadmap.

## Build and flash

Use ESP-IDF 5.5:

```bash
cd projects/orb
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

The project reuses the board component from `projects/spacepc-airstation/components/esp32_p4_wifi6_touch_lcd_4_3`.

## Project layout

```text
projects/orb/
├── main/
│   ├── main.c              display and service startup
│   ├── orb_ui.c            animated LVGL interface
│   ├── orb_data.c          providers, parsing and interpolation
│   ├── orb_map.c           Web Mercator tiles and microSD cache
│   ├── orb_net.c           serialized HTTPS access
│   ├── orb_route.c         on-demand origin/destination lookup
│   ├── orb_geocode.c       one-shot city geocoding
│   ├── orb_timezone.c      coordinate-based local-time offset
│   ├── orb_wifi.c          ESP32-C6 WiFi and NTP
│   └── orb_settings.c      local NVS configuration
├── docs/
├── partitions.csv
└── sdkconfig.defaults
```

## Status

This is an early functional prototype. Provider APIs are best-effort community services and must remain replaceable. Do not use Orb for navigation, collision avoidance or operational flight decisions.
