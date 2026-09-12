# Development

This guide describes the normal local workflow. Do not commit generated
firmware, archives, downloaded content, `sdkconfig`, or managed components.

## Requirements

- Python 3.10 or later
- `pytest` for host tests
- PlatformIO with `espressif32@7.0.1`, or ESP-IDF 6.0.1
- Swift 5.10 or later for the optional desktop flasher

## Test the host tools

```sh
python3 -m pytest -q
```

The suite checks archive validation, deterministic pack output, sanitization,
link rewriting, title-normalization parity, QR symbol encoding, and
reference-server routes.

## Build firmware

PlatformIO prepares the stylesheet, catalogue, and built-in gzip pack before
each build. It then stages flash-ready files in `firmware/dist/<environment>/`.

```sh
# 4 MB ESP32-C3
pio run -d firmware -e esp32-c3

# ESP32-S3 (the repository's 16 MB deployment model)
pio run -d firmware -e esp32-s3
```

The current PlatformIO board declaration reports an 8 MB, no-PSRAM
DevKitC-1. The 16 MB S3 layout is the intended deployment model, but must be
validated on the actual module before flashing. The pack-system benchmark
evidence is summarized in [the final recommendation](FINAL_RECOMMENDATION.md);
the live C3 serial measurements are in [DEVICE_BENCHMARK.md](DEVICE_BENCHMARK.md).

You can also use ESP-IDF:

```sh
source ~/esp/esp-idf/export.sh
idf.py -C firmware set-target esp32c3  # Use esp32s3 for the S3.
idf.py -C firmware build
```

The built-in archive comes from `articles/db-packs/biology-health/` in the
`pocketwiki-content` checkout, found at `../pocketwiki-content` unless
`POCKETWIKI_CONTENT_DIR` points elsewhere. To test a
different source directory with ESP-IDF, pass an absolute path:

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

## Preview the web interface

```sh
python3 tools/pack_content.py build ../pocketwiki-content/articles/db-packs/biology-health build/preview --codec gzip
python3 tools/reference_server.py build/preview --port 8080
```

The reference server helps you check routes and layout. It does not model
device memory, BLE, flash latency, or interrupted uploads.

The public landing page is [`pocketwiki3.html`](../pocketwiki3.html), the
BoardUI edition of the project overview. It is staged at both `/pocketwiki/`
(the canonical URL) and `/pocketwiki3/`; [`index.html`](../index.html) is the
earlier design and is only staged at `/pocketwiki2/`. The device's reader and
manager are separate local pages served by the firmware. When changing web copy,
update the firmware templates and reference server in the same change, then run
the host tests and preview both reader widths. To publish the landing page, run
`sh cloudflare/site/prepare.sh` followed by
`wrangler deploy --config cloudflare/site/wrangler.jsonc`.

The experimental PWPK browser path can be exercised with
`tools/pwpk_browser_server.py` and `/benchmark`; see
[BROWSER_BENCHMARK.md](BROWSER_BENCHMARK.md). It is separate from the
production built-in archive.

## Before you open a pull request

1. Run `python3 -m pytest -q`.
2. Build firmware if you changed `firmware/`, CMake, Kconfig, or partitions.
3. Preview HTML or CSS changes at desktop and phone widths.
4. Keep archive readers and writers compatible, or change the format version.
5. State the user-visible change and the checks you ran.
