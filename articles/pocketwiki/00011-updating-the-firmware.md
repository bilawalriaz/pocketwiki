# Updating the firmware

The firmware is the program on the board. It changes rarely: the packs you
install are what usually grow.

## Update in the browser

Open `educated.space/pocketwiki` in Chrome,
Edge, or another browser with WebSerial, connect the board over USB, and follow
the page. It downloads the current verified images, checks them against the
board, and writes them.

The page offers two modes:

- **Erase the whole flash first** (on by default): a clean slate. Installed
  packs, saved Wi-Fi, and all settings are cleared.
- **Off**: the firmware and built-in guide are updated in place, and the packs
  already on the board stay.

## Update over serial

From the project's repository:

```sh
pio run -d firmware -e esp32-c3        # or esp32-s3
python3 tools/flash_all.py --fw-dir firmware/dist/esp32-c3
```

Add `--no-firmware` to write only the built-in guide, or `--no-content` to
write only the firmware. For a full reset with esptool:

```sh
esptool --chip esp32c3 erase_flash
```

## Which board do I have?

Look at the module: the ESP32-C3 has one core and 4 MB of flash (about 1.9 MB
of pack storage); the ESP32-S3 has two cores and, in this project, a 16 MB
module (about 14.4 MB of pack storage). Flashing an image for the wrong board
is refused by the flasher, not by the board.
