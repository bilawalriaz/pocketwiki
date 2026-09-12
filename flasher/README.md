# espflasher

A small Swift toolchain for ESP32-family devices (ESP32, ESP32-C3, ESP32-S3,
and friends): detect what's plugged in, prepare the exact image (right
bootloader offset, right partition table, patched flash-size byte), and flash
it. Target macOS, written so the same core builds a Linux/Windows CLI.

Three pieces, one core:

| Target | What it is |
| --- | --- |
| `ESPFlashCore` | portable library: detection, partition tables, image building |
| `espflasher` | the CLI (macOS + Linux) |
| `ESPFlashGUI` | a thin macOS SwiftUI app over the same core |

## Why it shells out to esptool

Detection, partition-table generation, image merge, and bootloader patching
are implemented in Swift and byte-exact verified. The actual flash *write*
delegates to the standard **esptool** (same tool the repo's `flash_all.py`
uses): esptool handles reset, its stub flasher, the USB-Serial/JTAG path, and
verification, which is far more reliable over USB-JTAG than a hand-rolled ROM
writer. If esptool is not installed, the tool reports it clearly.

## Build

Requires Swift 5.10+ (macOS ships it; Linux needs the Swift toolchain).

```sh
swift build                      # macOS: core + CLI + GUI
swift build --product espflasher # Linux: core + CLI only
swift test
```

The GUI can be run directly (`swift run ESPFlashGUI`) or bundled into an app
with `scripts/bundle-macos.sh`.

The Device panel includes a locally bundled, coloured ESP32-S3 CAD preview.
It turns slowly while idle, pauses for direct click-drag inspection, ignores
scroll-wheel zoom so its framing stays stable, and respects macOS Reduce
Motion. The release bundle script also copies the USDZ resource bundle into
the app. Ready-to-run Apple-silicon and Intel builds are attached to each tagged
GitHub release.

## CI builds

`.github/workflows/macos-app.yml` builds and zips the app on GitHub's macOS
runners (arm64 + x86_64) on every push to `main` and pull request touching
`flasher/`. Tagging a release (`v*`) triggers `release.yml`, which also builds
the firmware images (`firmware/dist/<env>/` staged by `pio_post.py`) and
publishes everything — firmware zips for both boards plus the app zips — as
assets of the GitHub Release. The `VERSION` env var (the tag name) is written
into the app's `Info.plist` by `bundle-macos.sh`.

## Usage

```sh
# List serial ports with USB info
espflasher list [--json]

# Detect a board: chip family, revision, MAC, flash size
espflasher detect                # auto-scan
espflasher detect -p /dev/cu.usbmodem1101 --json

# Build a merged image without a device
espflasher image --chip esp32s3 --flash-size 16MB \
    --fw-dir ../firmware/dist/esp32-s3 --out merged.bin

# Prepare the right image and flash it
espflasher flash -p /dev/cu.usbmodem1101 \
    --partitions ../firmware/dist/esp32-s3/partitions.csv \
    --fw-dir ../firmware/dist/esp32-s3
```

`flash` resolves the plan automatically:

- **chip** is auto-detected; `--chip` overrides/validates.
- **flash size** is read from the flash chip (or eFuse); `--flash-size` overrides.
- **partition table** comes from `--partitions` (CSV), the build dir's
  `partitions.bin`/`partition-table.bin`, or a generated default layout.
- **bootloader** is discovered from `--fw-dir` and its flash-size byte is
  patched to match the detected flash only when `--flash-size` is explicit
  (otherwise the compiled value is kept, like esptool's default).
- **content/index** bins under `--fw-dir/content/` are written at their
  partition offsets.
- `--out` also writes a single merged image for later/esptool flashing.
- `--app-only` flashes just the app; `--erase-all` erases first; `--no-verify`
  skips esptool's verification.

```sh
# Generate or convert a partition table
espflasher partitions --chip esp32c3 --flash-size 4MB          # print CSV
espflasher partitions --chip esp32c3 --csv parts.csv --out parts.bin
```

## Porting to Linux / Windows

The core is pure Swift with no Apple-only imports outside the platform
enumerators:

- `Serial/SerialPort.swift` — termios; identical on macOS and Linux. A Windows
  port replaces just this file.
- `Serial/PortEnumerator.swift` — macOS uses IOKit, Linux uses `/sys/class/tty`,
  with a `/dev` fallback; add a Win32 enumerator for Windows.
- Everything else (protocol, partitions, image building) is portable.

Build the CLI on Linux with `swift build --product espflasher`; wrap the same
core in a WinUI/GTK/whatever GUI.

## Layout

```
Sources/ESPFlashCore/
  Serial/          termios port + enumerators
  Protocol/        ESP ROM protocol (sync, regs, chip detect, flash id)
  Detection/       detector: chip/revision/MAC/flash size + device lookup
  Image/           partition tables, firmware image, merge + bootloader patch
  Flasher/         esptool-backed flash + esptool locator
Sources/espflasher/    ArgumentParser CLI
Sources/ESPFlashGUI/   SwiftUI app
Tests/                 XCTest (partition byte-exactness vs a real build, etc.)
```
