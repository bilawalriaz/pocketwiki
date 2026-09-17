# Flashers

Three ways to write the firmware to a board: the flash panel on the landing
page, the Swift CLI, and the macOS app.

## Browser flashing

The flash panel on `https://educated.space/pocketwiki` writes the same images
with [`esptool-js`](https://github.com/espressif/esptool-js) over the Web Serial
API. Chrome and Edge on a desktop are the only browsers that expose a serial
port to a page; Firefox and Safari cannot. WebUSB is not an option for these
chips, because the operating system claims the ESP32-C3/S3 USB-serial peripheral
with its own CDC driver.

The panel asks for the board, optionally takes a 2.4 GHz network name and
password, erases the flash, and writes:

| Image | Offset (C3 / S3) |
| --- | --- |
| `bootloader.bin` | `0x0` |
| `partitions.bin` | `0x8000` |
| `firmware.bin` | `0x10000` |
| `content/content.bin` | `0x190000` |
| `content/index.bin` | `0x200000` / `0x210000` |
| `nvs` (Wi-Fi, only when asked) | `0x9000` |

The Wi-Fi step builds an NVS partition image in the browser, byte-compatible
with ESP-IDF's `nvs_partition_gen.py`, in the `pocketwiki` namespace the firmware
reads on boot. The flash size the panel writes comes from the chip itself, and
the bootloader's flash-size byte is patched to match, as esptool does. The panel
refuses to write when the detected chip or flash size does not match the chosen
layout.

Nothing is uploaded: the images come from the verified R2 firmware release, so
the page can flash without a local build. `tools/publish_firmware.py` checks
every image against its partition, uploads immutable versioned objects, and
updates the `firmware/index.json` release pointer with the offsets, byte counts
and SHA-256 digests the page verifies after fetching:

```sh
pio run -d firmware -e esp32-c3
pio run -d firmware -e esp32-s3
python3 tools/stage_web_firmware.py --check    # validate only
python3 tools/publish_firmware.py              # publish verified images to R2
```

`cloudflare/site/prepare.sh` still stages a local copy for offline preview, but
the deployed browser flasher reads the R2 manifest above.

## Swift flasher

`flasher/` provides a Swift command-line tool and a macOS app. They detect an
ESP32, select the matching image layout, and use esptool to write the device.

### Requirements

- Swift 5.10 or later
- `esptool` available through ESP-IDF, PlatformIO, or `pip install esptool`
- A firmware build staged in `firmware/dist/esp32-c3/` or
  `firmware/dist/esp32-s3/`

### Build and test

```sh
swift build --package-path flasher
swift test --package-path flasher
```

On macOS, start the app with `swift run --package-path flasher ESPFlashGUI`. Build an app
bundle with `flasher/scripts/bundle-macos.sh`.

### Flash a board

```sh
# ESP32-C3
flasher/.build/debug/espflasher flash \
  --partitions firmware/dist/esp32-c3/partitions.csv \
  --fw-dir firmware/dist/esp32-c3

# ESP32-S3
flasher/.build/debug/espflasher flash --port /dev/cu.usbmodemXXXX \
  --partitions firmware/dist/esp32-s3/partitions.csv \
  --fw-dir firmware/dist/esp32-s3
```

The tool detects the chip and flash size, checks each image, writes the images,
and asks esptool to verify the result.

Useful commands:

```sh
flasher/.build/debug/espflasher list
flasher/.build/debug/espflasher detect
flasher/.build/debug/espflasher partitions --chip esp32c3 --flash-size 4MB
```

Use `--app-only` to keep existing partitions, `--erase-all` to erase flash
before writing, and `--out merged.bin` to create a merged image. See
[`flasher/README.md`](../flasher/README.md) for command details.
