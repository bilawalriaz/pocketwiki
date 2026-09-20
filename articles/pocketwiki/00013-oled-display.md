# The OLED display

A small screen is optional. Without one the device works exactly the same; the
screen just saves you from guessing.

## What it shows

- On boot, the network name and the address to open.
- While reading, how many libraries and articles the device is serving and how
  much storage is left.
- During an install, the pack name and transfer progress.
- If you press the BOOT button, a QR code for joining the device's Wi-Fi
  network, and another for opening the reader. Scanning the first is usually
  the quickest way to connect a phone.

## Compatible screens

Any 128×64 SSD1306 over I2C. Both common addresses are tried: `0x3C` first,
then `0x3D`. The default pins are SDA on GPIO1 and SCL on GPIO0; on a custom
board move them to free pins, and avoid the pins the flash uses.

If no display is found, the device logs it and carries on. Nothing else
depends on the screen being there.

## Building without one

The display is a build option (`POCKETWIKI_OLED_ENABLE`). Turning it off frees
a little memory and the pins, which is useful when you are wiring something
else to the board.
