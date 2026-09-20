# Device benchmark

`tools/benchmark_device.py` drives the board's HTTP endpoints with a chosen
concurrency and reports latency, throughput, and device telemetry. It does not
flash or modify anything.

## Reproduction

With the board reachable (its own access point at `http://192.168.4.1`, or its
station address on the local network):

```sh
python3 tools/benchmark_device.py --target esp32c3 \
  --url http://192.168.4.1 --clients 1,2,5,10,20 --requests 20 --mode decoded
```

`--mode decoded` requests `/a/<id>`; `--pack <name>` selects an installed pack.
`/api/stats` is captured before and after each run. Keep the serial output of a
run when you are checking resets, watchdog events, or socket failures.

The figures below were recorded on the two reference boards. Treat them as a
baseline to compare against rather than as a specification: they depend on the
flash part, the compiler, and what else the firmware is doing at the time.

## ESP32-C3, 4 MB (10 September 2026)

ESP32-C3 revision 0.4 with 4 MB XMC flash, one 160 MHz core, no PSRAM,
ESP-IDF 6.0.1, and 216,084 bytes of reported total heap.

| Measurement | Result |
| --- | ---: |
| Reported total heap | 216,084 bytes |
| Largest free block at boot | 8,704 bytes |
| FAT read, 8 KiB buffer | 2,429,100 bytes in 712,814 µs |
| Raw flash read, 8 KiB buffer | 2,429,100 bytes in 440,642 µs |
| 16 KiB FAT/raw read buffer | Allocation failed |
| Pack-store usable geometry | 1,978,368 bytes |

A live install smoke test on the same board: a 343,943-byte pack installed over
the station uplink reached `P:343943:343943` and returned `OK:96`. Free heap was
45,876 bytes before the transfer, 97,464 bytes after Bluetooth was suspended,
and 45,184 bytes after it resumed.

## v3 article decoding on the ESP32-C3 (17 September 2026)

Every article is a raw DEFLATE stream that the device inflates out of its
static 32 KiB ring, after loading the pack's trained dictionary into that same
ring. Nothing was added to the image's static memory. Measured on a connected
ESP32-C3, with the firmware and the content and index partitions flashed from
this tree, and Bluetooth advertising plus the station uplink running:

| Quantity | Value |
| --- | ---: |
| app image (`firmware.bin`), current build | 1,224,416 B (78% of the 1.5 MB app partition) |
| static decoder (`s_decoder`, `nm`) | 44,784 B: the ring, the inflater state and the 1 KiB input chunk |
| built-in archive | 17 guide articles, 14,836 B payload + 4,096 B dictionary |
| free heap after boot (`/api/stats`) | 42,500 B (`heap_min` 31,512 B, largest block 29,696 B) |
| `/api/stats` | `format_version: 3`, `archive_ok: true`, `decoder_workspace_bytes: 44,784` |

Every response was compared byte-for-byte against the host decoder:

| Case | Result |
| --- | --- |
| built-in archive (flash partitions) | 100/100 articles identical, 10.2 s for the set |
| installed pack (FAT, 250,330 B upload) | install 3.1 s, 100/100 articles identical |
| pack installed from the published catalogue over the device's own HTTPS | `P:67235:67235`, `OK:24`, 24/24 articles identical |
| `ETag` / `If-None-Match` | 304, `crc-cd72b0f2` |
| `/download/0` | 200, `attachment; filename="pocketwiki-0.html"` |

Replacing an installed pack with a newly built one moved `pack_bytes_used` from
1,769,472 B to 1,662,976 B, as a 359,123 B pack became a 250,330 B one. The
per-request dictionary read costs 32 KiB of flash traffic per article, which the
read rates above put at 6 to 10 ms.

`tests/c_archive_decode_harness.c` reproduces the ring, the 1 KiB input
chunking and the dictionary convention for every article of the sample and
biology corpora, so a divergence fails in the host suite rather than on a board.

## ESP32-S3, 16 MB (17 September 2026)

A connected ESP32-S3 with a 16 MB QIO module and an SSD1306 display attached,
flashed from this tree after a full erase:

| Quantity | Value |
| --- | ---: |
| detected flash | 16 MB (`esptool flash_id`) |
| partition table | `partitions_16mb.csv` |
| boot log | `archive OK v3: 17 articles, payload 14836 bytes (dict 4096)` |
| pack store | `pack store ready: 0/14389248 bytes used`, matching the generated budget exactly |
| heap after boot (BLE + AP + httpd + OLED) | 101,952 B |
| steady state | `cpu 0% heap 95932` |
| display | SSD1306 detected at `0x3c` |

The S3 carries the same 44,784-byte static decoder as the C3. Its extra headroom
comes from DRAM rather than from a different code path.

## Pack density

`tools/benchmark_flash_budget.py` measures the shipping packer over the corpus
and writes `benchmarks/flash_budget.json`. It measures the per-domain packs
that ship and reports 2,388 bytes per article, which is about 825 articles in
the C3's pack store and 6,022 in the S3's. The same artifact records the
current image sizes: 1,224,416 bytes for the C3 and 1,144,240 for the S3, both
measured after the unused Zstandard decoder was dropped from
`sdkconfig.defaults`.

## Hardware-dependent gaps

- S3 Wi-Fi throughput, PSRAM use, and multi-client timings are unmeasured. The
  board used here reports no PSRAM, so the firmware is built for internal RAM.
- No 1/2/5/10/20-client load result is included, because the only live network
  probe timed out. The command above is ready to rerun when the board and the
  host share a reachable network.
