# Flashing

PocketWiki has two supported ways to write firmware: the browser flasher on
the landing page and the cross-platform Python script in `tools/`.

## Browser flashing

The flash panel on
[`educated.space/pocketwiki`](https://educated.space/pocketwiki) writes the
verified images with
[`esptool-js`](https://github.com/espressif/esptool-js) over the Web Serial
API. Chrome and Edge on a desktop are the only browsers that expose a serial
port to a page; Firefox and Safari cannot.

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

Nothing is uploaded: the images come from the verified R2 firmware release.
The page checks every image against the byte count and SHA-256 recorded in the
release manifest before writing it.

## Local flashing

Build the matching firmware target first, then install `esptool` through
ESP-IDF, PlatformIO, or `pip install esptool` and run the script:

```sh
# ESP32-C3
python3 tools/flash_all.py --port /dev/ttyACM0 \
  --fw-dir firmware/dist/esp32-c3

# ESP32-S3
python3 tools/flash_all.py --port /dev/ttyACM0 --chip esp32s3 \
  --partitions firmware/dist/esp32-s3/partitions.csv \
  --fw-dir firmware/dist/esp32-s3
```

The script checks that every image fits its partition before writing and
verifies the result. Use `--erase-all` for a completely clean reset, or
`--no-firmware` / `--no-content` when updating only one part of an existing
installation. Run `python3 tools/flash_all.py --help` for the full option list.

After the board starts, join the `PocketWiki` Wi-Fi network and open
`http://192.168.4.1/`.
