# SpacePC Weather Display

`spacepc-weather-display` ist eine eigenständige, quelloffene Firmware im
SpacePC-Firmware-Repository für genau diese Hardware:

- Good Display ESP32-L
- DESPI-C02
- Good Display GDEY075Z08, 7,5 Zoll, 800 × 480, Schwarz/Weiß/Rot

Das Projekt übernimmt keinen Quellcode, keine Grafiken und kein Layout aus dem
Repository `ESP32-e-Paper-Weather-Display`.

## Funktionen

- WLAN-Einrichtung über ein Captive Portal
- OpenWeatherMap mit aktuellem Wetter und 5-Tage-Vorschau
- Temperatur, Gefühlstemperatur, Luftfeuchte, Druck, Wind, Niederschlag und Sonnenzeiten
- Speicherung im NVS des ESP32
- modernes Schwarz-Weiß-Rot-Dashboard
- englische Setup- und Fehlerhinweise direkt auf dem E-Paper
- Deep Sleep mit konfigurierbarem Aktualisierungsintervall

## Ersteinrichtung

1. Firmware flashen.
2. Mit dem WLAN `SpacePC-Weather-<ID>` verbinden.
3. Das automatisch erscheinende Portal öffnen.
4. WLAN, Ort, Ländercode und OpenWeatherMap API-Key eintragen.

Der OpenWeatherMap API-Key wird im Captive Portal im Abschnitt
**OpenWeatherMap** eingetragen. Ein Key kann im OpenWeatherMap-Konto unter
**My API keys** erstellt werden; neue Keys sind eventuell nicht sofort aktiv.

## Bauen

```sh
cd projects/spacepc-weather-display
pio run
```

Flashen und seriell beobachten:

```sh
pio run --target upload --target monitor
```

## Lizenz

SpacePC Weather Display wird unter GPLv3 oder später veröffentlicht. Details und Hinweise für
den kommerziellen Vertrieb stehen in [docs/COMMERCIAL-GPL.md](docs/COMMERCIAL-GPL.md).
