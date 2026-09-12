# Architecture

PocketWiki has a content pipeline, firmware, and optional companion clients.

```text
Articles -> pack builder -> content.bin + index.bin -> ESP32 flash
                                                     -> local web reader
Phone or browser -> Wi-Fi or BLE -> validated .pwp -> internal pack store
```

## Content pipeline

`tools/pack_content.py` reads Markdown or HTML in two passes. It first finds
and normalizes titles, rejects duplicates, and assigns IDs. It then sanitizes
the articles, rewrites accepted internal links, compresses each article, and
writes a sorted fixed-width index.

New builds use gzip (`--codec gzip`) for predictable C3 memory use. Firmware
also reads v2 zstd packs for compatibility. See [Archive format](ARCHIVE_FORMAT.md)
for the byte layout.

The PWPK system is an experimental, separately versioned pack path. Its
browser integration serves compressed byte ranges and lets the browser decode
Zstd through the embedded WASM reader; the optional ESP-side service is
disabled by default and is not currently C3-ready. The PWPK format and
measurements are documented in [PWPK_FORMAT.md](PWPK_FORMAT.md),
[BROWSER_BENCHMARK.md](BROWSER_BENCHMARK.md), and
[FINAL_RECOMMENDATION.md](FINAL_RECOMMENDATION.md). This does not replace the
production `content.bin`/`index.bin` archives or the source database.

## Firmware startup

`app_main.c` initializes NVS, the optional OLED, AP+STA Wi-Fi, the built-in
archive, the pack store, BLE, and the HTTP server. A bad optional pack does not
stop the device. A bad built-in archive still allows diagnostic routes to run.

Key modules:

- `content_archive.c` validates and reads built-in and portable archives.
- `pack_store.c` manages the FAT-backed `/packs` filesystem and staged writes.
- `title_lookup.c` provides byte-stable normalization and binary search.
- `web_server.c` serves reading, search, Wi-Fi, and pack-management routes.
- `wifi_ap.c` manages the access point and saved station credentials.
- `ble_provisioning.c` provides setup and pack management over GATT.
- `oled_display.c` provides optional device status and transfer progress.

The web reader is local to the device. Its access-point address is
`http://192.168.4.1/`; station Wi-Fi is used only when the user asks the
device to refresh or download a catalogue pack.

## HTTP routes

| Route | Purpose |
| --- | --- |
| `GET /` | Browse the built-in and installed libraries. |
| `GET /search?q=` | Find exact and prefix title matches. |
| `GET /a/<id>?pack=<name>` | Read an article. |
| `GET /download/<id>?pack=<name>` | Download an article as HTML. |
| `GET /title/<title>` | Resolve an exact title. |
| `GET /browse` | Read a page of titles. |
| `GET /manage` | Manage Wi-Fi and packs. |
| `GET /packs` | Permanent compatibility redirect to `/manage`. |
| `GET /api/packs` | Get installed-pack data. |
| `GET /api/packs/catalog` | Get the local catalogue. |
| `POST /api/packs/catalog/sync` | Refresh the catalogue through station Wi-Fi. |
| `POST /api/packs/install` | Download and install a catalogue pack. |
| `POST /api/packs/upload` | Upload a `.pwp` file. |
| `POST /api/packs/action` | Remove a pack. |
| `GET /api/wifi/scan` | Scan 2.4 GHz networks. |
| `GET /api/wifi/status` | Get station and access-point status. |
| `POST /api/wifi/config` | Save station credentials. |
| `GET /api/stats` | Get device telemetry. |
| `GET /health` | Check service health. |

## Pack transfer

The browser uploads packs over Wi-Fi. The Android app uses BLE for discovery
and control. It sends pack bytes over Wi-Fi when it can reach the device and
falls back to acknowledged BLE chunks when it cannot.

Uploads write to a temporary file. The firmware checks the size, CRC, headers,
offsets, and index before it renames the file into `/packs`. The previous pack
remains available when validation fails.
