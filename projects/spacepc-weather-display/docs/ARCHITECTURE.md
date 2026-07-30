# Architektur

Die Firmware ist eine eigenständige OpenWeatherMap-Anzeige für den Good Display
ESP32-L, DESPI-C02 und das dreifarbige GDEY075Z08.

## Ablauf

1. `ConfigStore` lädt Ort, Ländercode, API-Key und Aktualisierungsintervall aus NVS.
2. `ProvisioningPortal` verbindet WLAN oder öffnet das lokale Captive Portal.
3. `WeatherClient` lädt aktuelles Wetter und die 3-Stunden-Vorschau.
4. Die Forecast-Slots werden zu Tages-Minima und -Maxima zusammengefasst.
5. `DisplayRenderer` zeichnet Dashboard, Windkompass und 36-Stunden-Graphen.
6. Der ESP32 schaltet WLAN und E-Paper ab und geht in Deep Sleep.

API-Keys und WLAN-Zugangsdaten bleiben lokal auf dem ESP32. HTTPS wird derzeit
ohne Zertifikats-Pinning verwendet.
