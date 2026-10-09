# Third-party components and services

BerryMaps is licensed under the GNU General Public License v3.0 (`LICENSE`).

| Component | Where | Used for | License |
|---|---|---|---|
| [mbed TLS](https://github.com/Mbed-TLS/mbedtls) 2.28 | from the sibling [BBport](https://github.com/damianolampisti00/bbport) project (`../BBport/third_party/mbedtls`) | TLS 1.2 to the map and Google services | Apache-2.0 |
| [libopus](https://opus-codec.org/) | `third_party/opus` (prebuilt armv7) | Decoding spoken guidance | BSD-3-Clause (`third_party/opus/COPYING`) |
| BlackBerry 10 Native SDK / Cascades | build toolchain, system libraries on the phone | UI framework and platform APIs | BlackBerry SDK license; linked as system libraries |

Online services (each user brings their own keys, see the README):

- Map data © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors (ODbL), map tiles © [CARTO](https://carto.com/attributions); the app shows this attribution on the map.
- Search, addresses, routes and transit times: Google Maps Platform (Places API, Geocoding, Routes), under Google's terms.
