# Device benchmark

`tools/benchmark_device.py` has two modes. The normal HTTP mode exercises the
same endpoints as the web UI without flashing. The serial mode builds a
temporary PWPK fixture, flashes only the application image, and captures
decoder, heap, storage, and board telemetry. It does not erase or rewrite the
content, index, or pack-store partitions.

## Reproduction

```sh
python3 tools/benchmark_device.py --serial-benchmark --target esp32c3 \
  --port /dev/cu.usbmodem1101 --output benchmarks/device_serial.json
```

For network load tests, connect to the board's AP or station address and run:

```sh
python3 tools/benchmark_device.py --target esp32c3 \
  --url http://192.168.4.1 --clients 1,2,5,10,20 --requests 20
```

`--mode decoded` requests `/a/<id>`; `--mode raw` requests `/raw/a/<id>` and
measures the browser-side compressed-byte path. `/api/stats` is captured before
and after each run. Serial monitor output should be retained when checking
resets, watchdog events, or socket failures.

## Measured C3 result (10 September 2026)

The board was an ESP32-C3 revision 0.4 with 4 MB XMC flash, one 160 MHz core,
no PSRAM, ESP-IDF 6.0.1, and 216,084 bytes reported total heap. The firmware
image was 1,470,464 bytes during the serial run; that historical fixture image
included the benchmark/prototype configuration. The current clean
production-default staged image is 1,226,528 bytes for C3 after removing those
test-only components and trimming unused mbedTLS roles/tools. The final image
also includes the C3 BLE suspend/resume lifecycle required for station-mode
HTTPS downloads. The serial fixture
and raw measurements are in
`benchmarks/device_serial.json` and its captured log.

| Measurement | Result |
| --- | ---: |
| FAT pack store total | 1,978,368 bytes |
| FAT pack store used during run | 270,336 bytes |
| FAT read, 8 KiB buffer | 2,429,100 bytes in 712,814 µs |
| Raw flash read, 8 KiB buffer | 2,429,100 bytes in 440,642 µs |
| 8 KiB buffer heap during read | 10,924 bytes free |
| 16 KiB buffer | allocation failed |
| Largest free block before decoder fixture | 8,704 bytes |

The standalone decoder fixture completed its benchmark task, but every Zstd
context allocation failed after normal Wi-Fi, OLED, BLE, HTTP, and pack-store
startup. This is a measured C3 integration limit, not a host-memory estimate;
the current ESP-side PWPK decoder path must not be described as C3-ready.

The host/browser path remains valid: the C3 can stream compressed bytes, and the
browser can decode them. The live station address printed by the board was
`192.168.0.213`. A later manual station-mode web install completed a
343,943-byte pack and is recorded in `benchmarks/device_install.json`; no
network concurrency or throughput result is claimed.

## Hardware-dependent gaps

- No ESP32-S3 board was connected. The S3 firmware build is verified, but S3
  heap, PSRAM, decoder, Wi-Fi concurrency, and raw-vs-FAT timings remain
  unmeasured.
- The current PlatformIO S3 board definition reports an 8 MB QD, no-PSRAM
  DevKitC-1. The repository's S3 partition table is a 16 MB deployment model;
  validate it on an actual 16 MB module before flashing.
- No 1/2/5/10/20-client board load result is included because the only live
  network probe timed out. The command is ready to rerun when the board and
  host share a reachable network.

Host allocator and symbol-size measurements in `benchmarks/decoder_memory.json`
and `benchmarks/decoder_code_size.json` are comparative evidence only. They are
not ESP SRAM or final linked-code measurements.
