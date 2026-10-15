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
- Read [docs/PACK_CATALOG.md](docs/PACK_CATALOG.md) before changing which
  articles a catalogue pack holds.

## Repository map

| Path | Purpose |
| --- | --- |
| `firmware/main/` | ESP-IDF firmware modules. |
| `articles/pocketwiki/` | Source of the built-in PocketWiki Guide. |
| `firmware/catalog/` | Published pack catalogue the firmware embeds. |
| `tools/` | Packer, sanitizer, preview server, simulator front end, and flash tools. |
| `sim/` | Host stand-ins for ESP-IDF that let the firmware run as a local device. |
| `tests/` | Python tests and C harnesses. |
| `case/` | 3D-printed case models. |

Article text for the optional packs is not in this repository. It is adapted
from Wikipedia and licensed CC BY-SA 4.0, so it lives in the companion
`pocketwiki-content` checkout, along with `packs/catalog-source.json`, the
shelf pool and its manifest, `packs/catalogue-selection.json`, and the generated
`packs.html` browser. `tools/content_paths.py` resolves that checkout, and the
catalogue tests skip when it is absent.

## Invariants

- Support ESP32-C3 with 4 MB flash and ESP32-S3 with 16 MB flash.
- Keep the built-in PocketWiki Guide archive recoverable from raw `content`
  and `index` partitions. It is never removable, and installing a pack of the
  same name updates it without a reflash.
- Keep the pack format deterministic and validate untrusted input twice: in
  the host tool and again on the device.
- Keep article decoding on-device and out of the heap: every article inflates
  through one static 32 KiB ring, and the pack's dictionary is read into that
  same ring rather than pinned in a second buffer.
- Keep Python and C title normalization byte-identical.
- Do not hold full optional archives or large buffers on task stacks.
- Keep optional-pack failure separate from the built-in reader.
- Keep HTTP article bodies identity-encoded. The device performs decoding.
- Do not fetch network content during a normal firmware build.
- Keep the shelves in `articles/cluster-packs/` and `packs.html` generated.
  Change the generator or `packs/cluster-labels.json` and re-run; never
  hand-edit their output.
- Keep clustered pack ids and versions stable across runs, so a published
  catalogue URL never changes meaning.
- Keep the published catalogue a subset of the shelf pool:
  `packs/catalogue-selection.json` names what ships, and a shelf id or name
  must stay under 48 bytes and a url under 96, or the device cannot read it.
- Keep the catalogue reader free of a fixed pack limit. A release may carry any
  number of packs; the device streams the document and holds one entry.

## Verification

Run `python3 -m pytest -q` for every change; it includes
`tests/test_device_sim.py`, which runs the firmware's web server, archive
reader and pack store on the host. To try a firmware change by hand, run
`python3 tools/pwsim.py run` and open `http://127.0.0.1:8080/`. Add a
simulator test for any route or pack-store behaviour you change. Build the
affected PlatformIO target when firmware, CMake, Kconfig, partition, or
embedded asset files change:

```sh
pio run -d firmware -e esp32-c3
pio run -d firmware -e esp32-s3
```

Do not commit generated firmware, archive files, downloaded corpora,
`sdkconfig`, managed components, or local build directories.
