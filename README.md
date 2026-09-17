# PocketWiki

PocketWiki turns an ESP32-C3 or ESP32-S3 into a small offline library. The
device creates its own Wi-Fi network and serves a searchable library at
`http://192.168.4.1/`. It can also join a 2.4 GHz network to download packs.

After setup, the device works without an account, phone, or internet connection
for reading.

## What is in the repository

- `firmware/` contains ESP-IDF firmware for ESP32-C3 and ESP32-S3 boards.
- `android/` contains the Kotlin and Compose companion app.
- `flasher/` contains the Swift CLI and the macOS app that write flash images.
- `tools/` contains the pack builder, the local preview server, the flash
  script, and the tools that stage firmware for the landing page.
- `assets/` contains the web assets the device serves and the landing-page
  screenshots.
- `case/` contains the 3D-printed case models.
- `articles/pocketwiki/` contains the built-in PocketWiki Guide.
- `docs/` contains the technical reference and development guide.

Article text for the optional packs is not here. It is adapted from English
Wikipedia and is licensed CC BY-SA 4.0, so it lives in the companion
[pocketwiki-content](https://github.com/bilawalriaz/pocketwiki-content)
repository together with the shelf pool, the pack manifest, and the generated
`packs.html` browser. Clone it beside this checkout; `tools/content_paths.py`
and the firmware build find it at `../pocketwiki-content`, or wherever
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
# Run the host tests. The catalogue tests need the content checkout.
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
optional, and the web interface remains the primary reader and manager.

## Create a pack

Each input article must have one `h1` title. Build, check, and wrap the pack:

```sh
python3 tools/pack_content.py build /path/to/articles build/my-pack
python3 tools/pack_content.py verify build/my-pack
python3 tools/pack_content.py pack-file build/my-pack my-pack.pwp
```

Every article is stored as an independent DEFLATE stream compressed against one
shared trained dictionary. The device loads that dictionary into the 32 KiB
inflate ring it already owns, so dictionary density costs no static RAM on
either board. Across the catalogue the average pack costs 2,388 bytes per
article, which puts roughly 825 articles in the ESP32-C3's 1.9 MB pack store
and roughly 6,022 in the S3's 13.9 MB; `tools/benchmark_flash_budget.py`
reproduces that measurement as `benchmarks/flash_budget.json`. One pack serves
every board. The packer removes active content and remote resources, rewrites
accepted internal links, and builds a sorted title index; the device checks a
pack before it installs it.

## Browse the catalogue

The catalogue browser is `packs.html` in the
[pocketwiki-content](https://github.com/bilawalriaz/pocketwiki-content)
repository. It opens from `file://` in any browser, with no server and no
network. It lists every pack with its collection, article count, and exact
install size, and searches inside the article titles each pack holds. Rebuild it
from this repository:

```sh
python3 tools/build_pack_browser.py   # writes packs.html into the content checkout
```

## Android app

Build the debug app with:

```sh
cd android
./gradlew assembleDebug
```

The app guides you through Device, Wi-Fi, and Library. It uses BLE for setup
and management. Pack bytes use Wi-Fi when possible and use BLE as a fallback.
Choose one or more packs from the catalogue and install them in one go: the
free space the device reports bounds the whole selection, and the app puts the
link back on its own if a transfer costs it.

## Read more

- [Development guide](docs/DEVELOPMENT.md)
- [System architecture](docs/ARCHITECTURE.md)
- [Hardware guide](docs/HARDWARE.md)
- [Archive format](docs/ARCHIVE_FORMAT.md)
- [Pack catalogue release guide](docs/PACK_CATALOG.md)
- [Device benchmark](docs/DEVICE_BENCHMARK.md)
- [Roadmap](docs/ROADMAP.md)
- [Swift flasher guide](docs/FLASHER.md)

Source code is MIT-licensed. Bundled Wikipedia-derived content retains the
attribution in each article and is available under CC BY-SA. See
[NOTICE](NOTICE) for the third-party components.
