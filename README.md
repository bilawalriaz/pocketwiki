# PocketWiki

PocketWiki turns an ESP32-C3 or ESP32-S3 into a small offline library. The
device creates its own Wi-Fi network and serves a searchable library at
`http://192.168.4.1/`. It can also join a 2.4 GHz network to download packs.

After setup, the device works without an account, phone, or internet connection
for reading.

## What is in the repository

- `firmware/` contains ESP-IDF firmware for ESP32-C3 and ESP32-S3 boards.
- `android/` contains the Kotlin and Compose companion app.
- `flasher/` contains the Swift CLI and macOS app that write flash images.
- `tools/` contains the pack builder, local preview server, flash script, and
  the tools that stage firmware for the landing page.
- `assets/` contains the web assets the device serves and the landing-page
  screenshots.
- `docs/` contains the technical reference and development guide.

**Article text is not in this repository.** The articles are adapted from
Wikipedia and are licensed CC BY-SA 4.0, which is not the license that covers
this code, so they live in the companion
[pocketwiki-content](https://github.com/bilawalriaz/pocketwiki-content)
repository. Clone it beside this checkout; `tools/content_paths.py` and the
firmware build find it at `../pocketwiki-content`, or wherever
`POCKETWIKI_CONTENT_DIR` points.

```sh
git clone https://github.com/bilawalriaz/pocketwiki.git
git clone https://github.com/bilawalriaz/pocketwiki-content.git
```

## Get started

You need Python 3.10 or later, PlatformIO or ESP-IDF 6, and a supported board.
For the shortest path, plug the board into USB and flash it from
[the landing page](https://educated.space/pocketwiki#flash) in Chrome or Edge,
which can also store your 2.4 GHz Wi-Fi details so the board is on your network
on its first boot. The rest of this section is the local build and flash
workflow.

```sh
# Run the host tests. Tests that need the article corpus skip when the
# pocketwiki-content checkout is absent.
python3 -m pytest -q

# Build one firmware target.
pio run -d firmware -e esp32-c3
# Or: pio run -d firmware -e esp32-s3

# Flash the C3 build. Give --port when auto-detection cannot find the board.
python3 tools/flash_all.py --fw-dir firmware/dist/esp32-c3
```

For an S3 board, use the matching staged build and partition table:

```sh
python3 tools/flash_all.py --port /dev/cu.usbmodemXXXX --chip esp32s3 \
  --partitions firmware/dist/esp32-s3/partitions.csv \
  --fw-dir firmware/dist/esp32-s3
```

After the board starts, join the `PocketWiki` Wi-Fi network and open
`http://192.168.4.1/`. The default network is open. Set a password before you
use the device in a shared place. Library installation and Wi-Fi settings are
available at `http://192.168.4.1/manage`. Reading does not need internet access;
the station Wi-Fi connection is only used for optional pack downloads and
catalogue refreshes.

`flash_all.py` writes the bootloader, partition table, firmware, and built-in
library. Use it for the first flash and when the built-in library changes.

To start an ESP32-S3 from zero, erase its entire flash first, then write the
matching 16 MB staged build. This removes optional packs and saved Wi-Fi
settings:

```sh
python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX erase-flash
python3 tools/flash_all.py --port /dev/cu.usbmodemXXXX --chip esp32s3 \
  --partitions firmware/dist/esp32-s3/partitions.csv \
  --fw-dir firmware/dist/esp32-s3
```

The landing page's **Erase the whole flash first** option performs the same
reset before downloading the current verified images from R2.

## Use the web interface

The device serves the reader at `/`. Use `/manage` to connect it to a 2.4 GHz
network, refresh the pack catalogue, install a `.pwp` file, or remove an
optional pack. The older `/packs` path redirects to `/manage` for compatibility.

If an SSD1306 OLED is connected, it shows the local reading address, the
current network address, article and pack counts, storage, and transfer
progress. A short press of the board's BOOT button cycles in a QR code for
joining the PocketWiki network and one for opening the reader. The OLED is
optional; the web interface remains the primary reader and manager.

## Create a pack

Each input article must have one `h1` title. Build, check, and wrap the pack:

```sh
python3 tools/pack_content.py build /path/to/articles build/my-pack --codec gzip
python3 tools/pack_content.py verify build/my-pack
python3 tools/pack_content.py pack-file build/my-pack my-pack.pwp
```

Gzip is recommended for the C3 because it has predictable memory use. The
packer defaults to zstd when `--codec` is omitted. Firmware reads both codecs.
The packer removes active content and remote resources, rewrites accepted
internal links, and builds a sorted title index. The device checks a pack
before it installs it.

To rebuild the published catalogue from the article sources, use the content
checkout:

```sh
python3 tools/build_pack_catalog.py            # reads ../pocketwiki-content
python3 tools/publish_pack_catalog.py          # uploads to R2
```

## Android app

Build the debug app with:

```sh
cd android
./gradlew assembleDebug
```

The app guides you through Device, Wi-Fi, and Library. It uses BLE for setup
and management. Pack bytes use Wi-Fi when possible and use BLE as a fallback.

## Read more

- [Development guide](docs/DEVELOPMENT.md)
- [System architecture](docs/ARCHITECTURE.md)
- [Hardware guide](docs/HARDWARE.md)
- [Archive format](docs/ARCHIVE_FORMAT.md)
- [PWPK benchmark and recommendation](docs/FINAL_RECOMMENDATION.md)
- [Pack catalogue release guide](docs/PACK_CATALOG.md)
- [Roadmap](docs/ROADMAP.md)
- [Swift flasher guide](docs/FLASHER.md)

## License

Source code and documentation are licensed under the Apache License 2.0
(© 2026 Bilawal Riaz). See [LICENSE](LICENSE) and [NOTICE](NOTICE) for the
license text and for the third-party components the firmware and apps ship.

Article text is a separate work. It is adapted from English Wikipedia, is
licensed CC BY-SA 4.0, and is kept in the companion
[pocketwiki-content](https://github.com/bilawalriaz/pocketwiki-content)
repository together with its attribution record.

PocketWiki is not affiliated with or endorsed by Espressif Systems, the
Wikimedia Foundation, Google, or Apple. See [NOTICE](NOTICE) for the trademark
statement.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Run `python3 -m pytest -q` before
opening a pull request.
