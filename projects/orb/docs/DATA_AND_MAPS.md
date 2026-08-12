# Orb data and maps

## Aircraft

The prototype uses the ADSB.lol point endpoint:

```text
GET https://api.adsb.lol/v2/point/{latitude}/{longitude}/{radius_nm}
```

It is free, open source and currently requires no key. Its published API permits a radius up to 250 nautical miles and describes the public data as ODbL. Orb derives the required radius from the visible map and clamps it to the same value. The provider abstraction exists because a public community endpoint has no production SLA and its access model may change.

Airplanes.live exposes a compatible point endpoint and is the planned fallback provider. It currently documents a one-request-per-second limit and non-commercial use for its unauthenticated API.

Selecting an aircraft first performs a callsign lookup through ADSBDB. If that database has no route, Orb asks ADSB.lol for a second lookup using the callsign and the aircraft's current latitude and longitude. The fallback accepts only a route that ADSB.lol marks plausible at that position, reducing incorrect matches when operational callsigns are reused. Orb displays the returned origin and destination airport codes and cities.

Neither provider supplies a scheduled or actual duration in these responses, so the value labelled `EST. DURATION` is deliberately only an estimate: great-circle airport distance at a representative cruise speed plus a fixed ground/climb allowance. Successful results are kept in a small RAM cache and are not persisted. Network failures are retried once and are never cached. A valid aircraft without a published route is shown as `NO SCHEDULED ROUTE`, which is expected for many private, local and unscheduled flights.

## Observer city

Settings accepts a city name as the map centre. Pressing Save triggers exactly one Nominatim search, then stores the resolved name and coordinates in NVS. There is no autocomplete, periodic geocoding or bulk lookup, requests are rate-limited to at most one per second, and a custom application User-Agent is sent. Manual coordinates remain available when the city field is empty.

Saving unchanged WiFi credentials does not reconnect the radio. A newly connected hosted-WiFi link may briefly have an IP address before DNS is ready, so city and time-zone lookups retry transient DNS/network startup failures with a delay.

After coordinates change, Orb resolves their IANA time-zone name and current UTC/DST offset through TimeAPI.io. NTP remains the source of absolute time; the stored offset is applied only for presentation. The lookup is refreshed every six hours so a continuously powered device crosses daylight-saving transitions correctly. The last successful zone and offset remain available offline.

This is appropriate for the interactive prototype under the public Nominatim usage policy. A distributed or commercial product must keep geocoding replaceable and use a suitable hosted or self-operated provider rather than depending on the community instance.

## ISS

`api.wheretheiss.at/v1/satellites/25544` returns latitude, longitude, altitude, velocity, visibility and timestamp without authentication. Its documented limit is roughly one request per second; Orb polls only once every ten seconds and animates between results.

## Other satellites

CelesTrak provides current GP elements as OMM JSON. Orb fetches the `VISUAL` group and propagates a small curated set locally. That gives smooth motion without continuously querying a server.

The V0.1 propagator is intentionally small and creates a useful globe visualization from mean elements. Accurate passes, azimuth/elevation and notifications require an embedded SGP4 implementation and release tests against a trusted reference implementation.

## Maps

The Sky view uses real OpenStreetMap Carto raster tiles in a fullscreen Web Mercator viewport. Aircraft and the saved observer are projected into the same coordinate system, so their markers stay geographically aligned while the user pans or zooms. Yellow aircraft symbols remain readable over the locally generated monochrome dark map; the selected aircraft and its callsign turn green.

The ISS view uses the same real map and cache at a global zoom level. Opening the page centers the viewport on the current station position. Once the ISS nears the edge, the viewport follows automatically and all recent trail samples are reprojected into the new map center instead of remaining at stale screen coordinates.

Orb follows the public tile-service requirements:

- only the active 3 x 5 portrait viewport is requested; there is no prefetch or bulk download mode;
- `(c) OpenStreetMap contributors` remains visible on the map;
- requests identify `SpacePC-Orb` with a project URL and contact address;
- every downloaded PNG is cached persistently on microSD and reused indefinitely;
- without a mounted microSD, Orb does not request public map tiles.

The cache layout is `/sdcard/ORB/MAPS/{z}/{x}/{y}.png`. A background task decodes each PNG once, converts it locally into a dark RGB565 tile in PSRAM and hands the native pixels to LVGL. This avoids frame-time PNG decoding and does not introduce a separate dark-map provider. HTTPS operations for maps, aircraft, ISS and CelesTrak share one mutex; this limits peak TLS memory and protects the hosted WiFi transport from simultaneous large requests.

Aircraft queries follow the visible map. Orb calculates the radius from the current Web Mercator scale and the viewport diagonal, adds a small edge margin and clamps the public query to the provider's 10–250 nautical-mile range. Panning or zooming refreshes the query around the new map center; no independent range control exists in Settings.

For a commercial release or fleet deployment, the tile URL remains a provider boundary. A product-owned tile service or a licensed offline regional archive should replace the community endpoint so the product does not rely on a best-effort public service.

## Security and privacy

- WiFi credentials, observer city, coordinates, time zone, current UTC offset and preferred units are stored only in the device's NVS partition.
- Live provider requests disclose the configured observer coordinates to the aircraft API because the regional query requires them.
- Map requests reveal the requested tile coordinates to the map service, which approximately identify the visible area.
- No Orb account or SpacePC cloud is involved.
- TLS uses the ESP-IDF certificate bundle.
