# BerryMaps

Native maps app for BlackBerry 10 (tested on a Q5): OpenStreetMap base map (CARTO
Voyager @2x), GPS position, Google Places search, Google Routes directions and
turn-by-turn guidance (car, walk, bike, public transport). Design notes, measurements
and decisions (in Italian): [PROGETTO.md](PROGETTO.md).

## Layout

- `app/` – BerryMaps (Cascades, Qt 4.8). `powershell -File app\package.ps1 -Install`
  builds, packages and installs it on the phone over SSH (root).
- `probe/` – BerryProbe, the "Fase 0" hardware diagnostic app.
- `tools/installbar.sh` – unsigned .bar installer run on the phone as root.

## Build requirements

- BlackBerry 10 NDK 10.3.1 in `C:\bbndk` (the packager needs its bundled JRE 1.7).
- mbedTLS (TLS 1.2) from the sibling `BBport` project:
  `../BBport/third_party/mbedtls` (see `app/BerryMaps.pro`).

## Keys (never in the repository)

One-line files on the phone, read at startup:

- `/accounts/1000/shared/misc/berrymaps_cartokey.txt` – CARTO basemaps key
- `/accounts/1000/shared/misc/berrymaps_apikey.txt` – Google Maps Platform key
  (Places API (New), Geocoding, Routes)

Log: `/accounts/1000/shared/misc/berrymaps.log`.
