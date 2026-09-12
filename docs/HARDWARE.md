# Hardware

## Supported boards

| Board | Deployment model | PlatformIO environment | Partition table |
| --- | ---: | --- | --- |
| ESP32-C3 DevKitM-1 compatible | 4 MB | `esp32-c3` | `partitions.csv` |
| ESP32-S3 module with 16 MB flash | 16 MB | `esp32-s3` | `partitions_16mb.csv` |

Both chips support 2.4 GHz Wi-Fi only. In AP+STA mode, the access point and
station share one radio and therefore one channel.

The current PlatformIO `esp32-s3` board declaration reports an 8 MB QD,
no-PSRAM DevKitC-1. The firmware and partition table are intended for a
16 MB S3 deployment, but that capacity and any PSRAM must be confirmed on the
actual module; they are not measured facts for the current build host.

## Display wiring

PocketWiki supports a 128 x 64 SSD1306 OLED over I2C.

| Signal | Default pin |
| --- | --- |
| SDA | GPIO1 |
| SCL | GPIO0 |
| I2C address | `0x3C`, then `0x3D` |

The display is optional. If the firmware cannot find it, the library and web
server still work. On an ESP32-S3, GPIO0 is a boot strap pin; keep it high
during reset. For a custom S3 board, GPIO1 for SDA and GPIO2 for SCL are safer
choices. Do not use flash/PSRAM pins or other strapping pins without checking
the module design.

## Boot button

The board's BOOT button cycles the OLED between the live status and the QR
screens, so a phone can join the network and reach the reader without typing
anything.

| Board | BOOT button | Notes |
| --- | --- | --- |
| ESP32-C3-DevKitM-1 | GPIO9 | Free on the default wiring. |
| ESP32-S3-DevKitC-1 | GPIO0 | The default OLED SCL pin is also GPIO0. |

The default is per target (`POCKETWIKI_BUTTON_GPIO`: 9 on the C3, 0 on the
S3), and both are active low with the internal pull-up enabled. If the button
GPIO equals the OLED SDA or SCL pin, the firmware logs the conflict and
disables the button rather than sharing the pin with I2C traffic. On the S3,
move the display to the safer SDA 1 / SCL 2 pair (or set
`POCKETWIKI_BUTTON_GPIO` to another free pin) to use the button.
`POCKETWIKI_BUTTON_ENABLE=n` removes the feature entirely.

## Flash layout

The C3 layout reserves a 1.5 MiB app partition, 448 KiB for built-in content,
64 KiB for its index, and a 1,978,368-byte FAT-usable installed-pack store
before the 8 KiB safety reserve. The 16 MB S3 model provides 14,389,248 FAT
usable bytes and 14,381,056 bytes after that reserve. These values are generated
from the repository partition tables with IDF FAT geometry; confirm mounted
capacity on the target module.

The current clean production-default staged firmware images measure 1,226,528
bytes (C3) and 1,156,608 bytes (S3). The C3 app therefore has 347,020 bytes of
image headroom; the S3 model has 416,685 bytes. The embedded PWPK browser
assets are not included in these production images. If the experimental
browser decoder service is enabled, its assets must fit inside the app
partition and must not be subtracted a second time from the pack store. The
serial benchmark is disabled in these production images.

The `packs` partition uses FAT with wear levelling. See
[FINAL_RECOMMENDATION.md](FINAL_RECOMMENDATION.md) for article-density
estimates and their assumptions.

## First boot

1. Flash the board with the command in the [development guide](DEVELOPMENT.md).
2. Join the `PocketWiki` network — with an OLED attached, press BOOT once for
   a QR code that offers to join it, or twice for the reader address.
3. Open `http://192.168.4.1/`.
4. Use `/manage` to set station Wi-Fi or install a pack.

The default access point is open. Configure a WPA2 password before shared use.

## OLED status and transfer UX

When an SSD1306 is detected, the normal status screen keeps the information
needed to use the device nearby:

- `READ: 192.168.4.1` is the address for the local reader and manager.
- `OPEN:` shows the access-point address; `LAN:` shows the station address when
  the device is also connected to another Wi-Fi network.
- `ART / PACK` shows the article and installed-pack counts, followed by a
  storage bar.

During a pack transfer, the OLED switches to the pack name and percentage. On
boot it shows the local address so a user can begin without a serial console.
If the built-in archive is invalid, it shows `LIBRARY ERROR` while the
diagnostic web routes remain available.

### Boot-button screens

Each short BOOT press advances one step and wraps around:

1. Live status (the default screen).
2. **Join Wi-Fi** — a full-screen QR code holding the `WIFI:` payload for the
   access point. A phone camera offers to join the network from this code.
3. **Reader** — a QR code for `http://<AP IP>/`, so the reader opens once the
   phone is on the network.

The symbol is drawn dark-on-light at two pixels per module on a lit card, with
the panel otherwise dark: the card supplies the quiet zone the decoder needs,
and keeping the lit area small makes a phone meter for the symbol instead of a
screenful of light.

A transfer owns the display, so presses during an upload are ignored. The
join symbol is skipped when its payload cannot fit a scannable symbol — it
needs a version 4 or larger symbol, which with the default SSID means an
access-point password longer than about twenty-five characters — and the press
advances to the reader code instead. The firmware logs the reason at that
point.

