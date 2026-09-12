# PocketWiki contributor guide

PocketWiki is an offline library for ESP32-C3 and ESP32-S3 boards. The device
serves articles from local flash, manages optional `.pwp` packs, and provides a
web interface, BLE setup, and an optional SSD1306 OLED.

## Start here

- Read [README.md](README.md) for the normal build and flash workflow.
- Read [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) before changing the project.
- Read [docs/ARCHIVE_FORMAT.md](docs/ARCHIVE_FORMAT.md) before changing pack
  tooling, archive readers, or title lookup.
- Read [docs/HARDWARE.md](docs/HARDWARE.md) before changing pins or partitions.

## Repository map

| Path | Purpose |
| --- | --- |
| `firmware/main/` | ESP-IDF firmware modules. |
| `android/` | Android companion app. |
| `../pocketwiki-content` | Article text and pack manifest (separate CC BY-SA repository). |
| `tools/` | Packer, sanitizer, preview server, and flash tools. |
| `tests/` | Python tests and C harnesses. |
| `flasher/` | Swift CLI and macOS flasher. |

## Invariants

- Support ESP32-C3 with 4 MB flash and ESP32-S3 with 16 MB flash.
- Keep the built-in Biology, Health & Medicine archive recoverable from raw
  `content` and `index` partitions.
- Keep the pack format deterministic and validate untrusted input twice: in
  the host tool and again on the device.
- Keep Python and C title normalization byte-identical.
- Do not hold full optional archives or large buffers on task stacks.
- Keep optional-pack failure separate from the built-in reader.
- Keep HTTP article bodies identity-encoded. The device performs decoding.
- Do not fetch network content during a normal firmware build.
- Read article text from the separate `pocketwiki-content` checkout through
  `tools/content_paths.py`; keep article text out of this repository.

## Verification

Run `python3 -m pytest -q` for every change. Build the affected PlatformIO
target when firmware, CMake, Kconfig, partition, or embedded asset files change:

```sh
pio run -d firmware -e esp32-c3
pio run -d firmware -e esp32-s3
```

Do not commit generated firmware, archive files, downloaded corpora,
`sdkconfig`, managed components, or local build directories.
