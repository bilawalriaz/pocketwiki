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

There is one codec: every article is a raw DEFLATE stream compressed against a
shared trained dictionary. DEFLATE's LZ window *is* the ring the article
decoder inflates through, so the server loads the dictionary into that ring
before each article and dictionary compression costs no additional RAM: the
ESP32-C3 gets it while keeping the Bluetooth link the Android app depends on,
and one pack serves both boards. See
[Archive format](ARCHIVE_FORMAT.md) for the byte layout and the device limits.

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

## Web response budget

`esp_http_server` serves one request at a time, so a response that stalls stops
every other route on the device. Three rules keep the reader reachable on the
4 MB C3, which is the tighter target:

- Body chunks go out in `PW_CHUNK_SIZE` (1 KiB) pieces through `send_body()`,
  including page sections and the embedded manager script. Chunk boundaries are
  arbitrary, so splitting a rendered buffer changes nothing on the wire.
- A chunked response ends with `finish_response()`, which writes the 5-byte
  terminator in one bounded socket write. ESP-IDF's own terminator path retries
  partial writes and can stall the server task on the ESP32-C3; a handler that
  streamed no body at all instead ends with `httpd_resp_send(req, NULL, 0)` so
  the headers still precede the terminator.
- Every write failure retires the client (`retire_client()`). A client that
  stops reading - a phone browser that gave up on a page, a connection left
  behind by a cancelled load - otherwise blocks the single handler task, and
  with it every other route, until the socket errors. Measured on an ESP32-C3:
  four abandoned page loads left the server unreachable for as long as it was
  left running; with the retirement it recovered immediately.
- `send_wait_timeout` is 5 s. Handlers are serialized, so this value is how
  long one unresponsive client can hold the whole server.
- The socket pool is 7 (`CONFIG_POCKETWIKI_HTTPD_MAX_SOCKETS`) with
  `CONFIG_LWIP_MAX_SOCKETS=12`, because a browser opens several parallel
  connections and the server purges - truncating - a live response when the
  pool is exhausted.

## Pack transfer

The browser uploads packs over Wi-Fi. The Android app uses BLE for discovery
and control. It sends pack bytes over Wi-Fi when it can reach the device and
falls back to acknowledged BLE chunks when it cannot.

A pack install is a session, not a request: the device accepts one upload at a
time and reports the space it has left. A client that installs several packs
therefore queues them and checks each against the space the previous one left,
rather than starting uploads the device would have to refuse.

The app has to fetch before it installs, and as one batch. Reaching the
device's access point takes the phone off the internet, so it downloads the
whole selection first and only then installs it, over one connection that is
held open for the batch. A download started between two installs would have
nowhere to come from.

Uploads write to a temporary file. The firmware checks the size, CRC, headers,
offsets, and index before it renames the file into `/packs`. The previous pack
remains available when validation fails.
