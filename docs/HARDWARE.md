# Hardware

## Supported boards

| Board | Deployment model | PlatformIO environment | Partition table |
| --- | ---: | --- | --- |
| ESP32-C3 DevKitM-1 compatible | 4 MB | `esp32-c3` | `partitions.csv` |
| ESP32-S3 module with 16 MB flash | 16 MB | `esp32-s3` | `partitions_16mb.csv` |

Both chips support 2.4 GHz Wi-Fi only. In AP+STA mode, the access point and
station share one radio and therefore one channel.

Sharing one radio also means a station connect attempt takes the access point
off the air: `esp_wifi_connect()` scans channels before it associates. The
firmware therefore paces retries after a disconnect (1 s, doubling to a 30 s
cap, reset once the station has an address) instead of retrying as fast as the
driver reports failures. A stale or unreachable saved network otherwise keeps
the access point from serving `http://192.168.4.1/`, which is how the reader is
reached. Each retry is logged with the driver's disconnect reason.

A station link that is connected but at the edge of range is its own problem:
the radio spends its airtime on the station's retries and rate adaptation, and
the access point's own clients are served from what is left. A C3 measured at
-90 dBm served the reader page once or twice and then stopped answering on both
iOS and Android, while the same board with no uplink served it indefinitely.
The firmware therefore drops an uplink that stays below
`CONFIG_POCKETWIKI_STA_MIN_RSSI` (default -87 dBm) for a few seconds: the saved
credentials are kept, the reason is logged, and `/manage` can set the Wi-Fi
again to retry. A pack transfer in progress is never interrupted. Pick a host
board or place the device so its uplink is comfortably above that level, or
leave the uplink unset and sideload packs.

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

The current staged firmware images measure 1,224,416 bytes (C3) and 1,144,240
bytes (S3). The C3 app therefore has 348,448 bytes of image headroom; the S3
has 428,624 bytes.

The `packs` partition uses FAT with wear levelling. Measured pack density and
what that puts in each store is in [DEVICE_BENCHMARK.md](DEVICE_BENCHMARK.md).

## First boot

1. Flash the board with the command in the [development guide](DEVELOPMENT.md).
2. Join the `PocketWiki` network. With an OLED attached, press BOOT once for
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
2. **Join Wi-Fi**: a full-screen QR code holding the `WIFI:` payload for the
   access point. A phone camera offers to join the network from this code.
3. **Reader**: a QR code for `http://<AP IP>/`, so the reader opens once the
   phone is on the network.

The symbol is drawn dark-on-light at two pixels per module on a lit card, with
the panel otherwise dark: the card supplies the quiet zone the decoder needs,
and keeping the lit area small makes a phone meter for the symbol instead of a
screenful of light.

A transfer owns the display, so presses during an upload are ignored. The
join symbol is skipped when its payload cannot fit a scannable symbol. It needs
a version 4 or larger symbol, which with the default SSID means an
access-point password longer than about twenty-five characters. The press then
advances to the reader code instead. The firmware logs the reason at that
point.

