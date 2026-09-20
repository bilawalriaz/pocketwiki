# Development

This guide describes the normal local workflow. Do not commit generated
firmware, archives, downloaded content, `sdkconfig`, or managed components.

## Requirements

- Python 3.10 or later
- `pytest` for host tests
- PlatformIO with `espressif32@7.0.1`, or ESP-IDF 6.0.1

Clone the content repository beside this one, or point `POCKETWIKI_CONTENT_DIR`
at it. The pack catalogue, the article text it names, and the catalogue browser
all live there; see `tools/content_paths.py`. The catalogue tests fail with a
clear message when it is missing, and the rest of the suite still runs.

```sh
git clone https://github.com/bilawalriaz/pocketwiki-content.git
```

## Test the host tools

```sh
python3 -m pytest -q
```

The suite checks archive validation, deterministic pack output, sanitization,
link rewriting, title-normalization parity, QR symbol encoding, catalogue
parsing, and reference-server routes. Tests that need the article corpus skip
when the content checkout is not present.

## Build firmware

PlatformIO prepares the stylesheet, catalogue, and built-in pack before each
build, then stages flash-ready files in `firmware/dist/<environment>/`.

```sh
# 4 MB ESP32-C3
pio run -d firmware -e esp32-c3

# 16 MB ESP32-S3
pio run -d firmware -e esp32-s3
```

The current PlatformIO board declaration reports an 8 MB, no-PSRAM DevKitC-1.
The 16 MB S3 layout is the deployment model and has been validated on a 16 MB
module; check the module before flashing something else. Measured capacity and
image sizes for both boards are in [DEVICE_BENCHMARK.md](DEVICE_BENCHMARK.md).

You can also use ESP-IDF:

```sh
source ~/esp/esp-idf/export.sh
idf.py -C firmware set-target esp32c3  # Use esp32s3 for the S3.
idf.py -C firmware build
```

The built-in archive is the PocketWiki Guide, packed from
`articles/pocketwiki/`. To test a different source directory with ESP-IDF, pass
an absolute path:

```sh
idf.py -C firmware -DPW_ARTICLES_DIR=/absolute/path/to/articles build
```

## Flash a board

Build first. The arguments must match the board and its partition table.

```sh
# ESP32-C3
python3 tools/flash_all.py --port /dev/ttyACM0 \
  --fw-dir firmware/dist/esp32-c3

# ESP32-S3
python3 tools/flash_all.py --port /dev/cu.usbmodemXXXX --chip esp32s3 \
  --partitions firmware/dist/esp32-s3/partitions.csv \
  --fw-dir firmware/dist/esp32-s3
```

The script checks that each image fits its partition before it calls esptool.
Use `--no-firmware` to write only content and index. Use `--no-content` to
write only the bootloader, partition table, and application.

For a fresh ESP32-S3 setup, erase the complete flash before running the normal
flash command. This intentionally removes optional packs and saved Wi-Fi
credentials:

```sh
python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX erase-flash
python3 tools/flash_all.py --port /dev/cu.usbmodemXXXX --chip esp32s3 \
  --partitions firmware/dist/esp32-s3/partitions.csv \
  --fw-dir firmware/dist/esp32-s3
```

## Browse the pack catalogue

```sh
python3 tools/build_pack_browser.py    # rebuild packs.html in the content checkout
python3 tools/publish_pack_catalog.py  # build, verify, and upload the release
```

`packs.html` is one self-contained page for browsing and sorting the catalogue:
every pack, its collection, article count, and exact install size, with the
article titles inside each pack. It opens from `file://` with no server. The
tool builds every pack to measure it, so it also fails on a pack the device
could not install. See [PACK_CATALOG.md](PACK_CATALOG.md) for the shelf pool,
the release selection, and the labels file.

## Preview the web interface

```sh
python3 tools/pack_content.py build articles/pocketwiki build/preview
python3 tools/reference_server.py build/preview --port 8080
```

The reference server helps you check routes and layout. It does not model
device memory, BLE, flash latency, or interrupted uploads.

The public landing page is [`pocketwiki.html`](../pocketwiki.html), the BoardUI
edition of the project overview, staged as the canonical `/pocketwiki/`. The
device's reader and manager are separate local pages served by the firmware,
and the landing hero previews them at the reader's own sizes: its markup and
styles are copied from `firmware/main/web_server.c` and `assets/style.css`, so
a change to either belongs in this file too. When changing web copy, update the
firmware templates and reference server in the same change, then run the host
tests and preview both reader widths. To publish the landing page, run
`sh cloudflare/site/prepare.sh` followed by
`wrangler deploy --config cloudflare/site/wrangler.jsonc`.

## Before you open a pull request

1. Run `python3 -m pytest -q`.
2. Build firmware if you changed `firmware/`, CMake, Kconfig, or partitions.
3. Preview HTML or CSS changes at desktop and phone widths.
4. Keep archive readers and writers compatible, or change the format version.
5. State the user-visible change and the checks you ran.
