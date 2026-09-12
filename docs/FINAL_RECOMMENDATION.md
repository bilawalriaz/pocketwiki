# PocketWiki pack-system final recommendation

Date: 10 September 2026

## Decision

Keep the production archive path unchanged. The built-in Biology, Health &
Medicine archive, the source database, and the existing `content.bin`/
`index.bin` production artifacts remain the recoverable deployment baseline.
The current production guidance—gzip for predictable C3 memory use, with
firmware compatibility for v2 Zstd—still applies.

Treat PWPK as an experimental browser-first path:

1. Stream compressed article ranges from the device and decode in the browser.
   Use independent Zstd frames at level 19 as the interoperable default.
2. Permit a pack-specific 64 KiB dictionary only for packs whose browser
   transfer is measured and whose client uses the WASM adapter. Native browser
   `DecompressionStream` has no dictionary argument; dictionary packs therefore
   require the WASM path.
3. Keep the ESP-side PWPK decoder service and serial benchmark disabled in
   production images. The normal pack manager, web UI, BLE upload path, and
   `/a/<id>` article serving do not depend on those experimental/test-only
   components; enable the decoder service only for its dedicated browser
   experiment. The connected C3 could not allocate any tested Zstd decoder
   context after normal services started, so a static-context or larger-memory
   redesign needs a new device measurement before changing this decision.
4. Keep device-side catalogue/pack downloads on the station interface only.
   The production TLS client accepts the standard 16 KiB incoming record size
   required by arbitrary HTTPS pack hosts, uses dynamic RX buffers for pack
   downloads, and still frees handshake-only certificate/configuration data.
   On the C3, NimBLE is suspended only for the station HTTPS transfer and
   restarted before the response completes; a live BLE session is rejected
   with a clear retryable error. The browser can use the AP while the device
   fetches over its configured internet uplink. Android/BLE remains the
   fallback when the station is disconnected.
5. Do not select 256 KiB semantic blocks for C3. They were the best full-host
   Zstd density result, but they increase random-access amplification and have
   a 486,537-byte host allocator peak in the decoder experiment. Brotli's
   smaller host result is not deployable because the firmware and current PWPK
   browser adapter do not provide a Brotli route.

## Evidence and fingerprints

The host matrix contains 663 completed cases. Every row is `ok`, and the rows
carry the current compressor and corpus implementation fingerprints. The
corpus is 21,966 articles and 104,825,233 raw bytes; the source and embedding
hashes are recorded in `benchmarks/corpus.json`. The matrix is preserved in
`benchmarks/results.json`, `benchmarks/results.csv`, and the generated
`benchmarks/COMPRESSION_BENCHMARK.md`.

The most relevant full-corpus host results are:

| Strategy | Pack bytes | Bytes/article | Important limitation |
| --- | ---: | ---: | --- |
| Brotli 11, 64 KiB semantic blocks | 36,553,168 | 1,664.08 | No firmware decoder in this project |
| Zstd 19, 256 KiB semantic blocks | 37,694,844 | 1,716.05 | 486,537-byte host decoder peak; ~54.38× read amplification |
| Zstd 19, 64 KiB semantic blocks | 41,378,598 | 1,883.76 | ~13.22× read amplification |
| Zstd 19, independent, 64 KiB pack dictionary | 39,439,668 | 1,795.49 | WASM dictionary path; transfer must be measured per pack |
| Zstd 19, independent, no dictionary | 50,184,769 | 2,284.66 | Simplest random-access model |

These are host measurements, not claims about ESP SRAM or Wi-Fi throughput.
The matrix's pack dictionaries improve density, but they do not overcome the
C3 decoder allocation limit.

## Browser verification

The live Chromium verification succeeded for 50/50 requests in each current
fixture: independent Zstd, 64 KiB blocks, and a 16 KiB dictionary fixture. The
measured median/P95 decode times were 0.1/0.2 ms, 0.2/2.1 ms, and 0.1/0.2 ms,
respectively. The native `Content-Encoding: zstd` probe also decoded 5,898
bytes successfully. These numbers are browser-version evidence, not a
universal compatibility guarantee; keep feature detection and the WASM
fallback.

The fallback asset set is 278,877 bytes raw and 89,701 bytes gzip-compressed
for transfer; the Zstd WASM module is 251,806 raw bytes. The actual linked
firmware image is the authoritative embedded-flash cost.

## Device measurements

The available board is an ESP32-C3 revision 0.4 with 4 MB XMC flash, one
160 MHz core, no PSRAM, and ESP-IDF 6.0.1. The serial fixture was flashed as
an application-only test; content, index, and pack-store partitions were not
erased or rewritten.

| Measurement | Result |
| --- | ---: |
| Reported total heap | 216,084 bytes |
| Largest free block before decoder fixture | 8,704 bytes |
| Zstd context allocations | 0/7 succeeded |
| FAT read, 8 KiB buffer | 2,429,100 bytes in 712,814 µs |
| Raw-app read, 8 KiB buffer | 2,429,100 bytes in 440,642 µs |
| 16 KiB FAT/raw read buffer | Allocation failed |
| Pack-store usable geometry | 1,978,368 bytes |

The complete raw result is `benchmarks/device_serial.json`, with the serial
log in `benchmarks/device_serial.log`. The C3 station address was reachable
during the follow-up probe, but the available link was too slow for a complete
pack-install timing measurement; no throughput or concurrency result is
claimed. A separate live install smoke-test record is preserved in
`benchmarks/device_install.json`.

The same board subsequently completed a station-mode web install of the
343,943-byte `philosophy-religion-ethics-v1.pwp` pack: progress reached
`P:343943:343943` and the device returned `OK:96`. Free heap measured 45,876
bytes before the transfer, 97,464 bytes after BLE was suspended, and 45,184
bytes after BLE resumed; the largest free block during the transfer was
47,104 bytes. This validates completion and recovery, but not throughput.
The device then reported 1,929,216 of 1,978,368 pack-store bytes used, 40,960
bytes installable, and 576 installed articles across six 96-article entries.
The sub-600 result is therefore the current pack selection's compressed-byte
density plus FAT cluster/reserve overhead; it is not a fixed article-count
limit, and a different pack mix will produce a different count.

## Refreshed resource and flash budgets

The current clean production-default staged firmware images measure 1,226,528
bytes for C3 and 1,156,608 bytes for S3 (PlatformIO linked application sizes:
1,225,844 and 1,156,179 bytes). The earlier 1,218,288/1,148,528 figures are
stale because the final images include the release web-manager copy, OLED UX,
and regenerated embedded assets in addition to the BLE suspend/resume lifecycle
needed for reliable C3 station downloads. The serial benchmark and experimental
browser decoder remain disabled; normal HTTPS client validation, Wi-Fi, BLE,
pack management, and article serving remain enabled.
The generated flash budget records the filesystem geometry, browser asset size,
and an 8,192-byte safety reserve:

| Target model | Pack partition | FAT usable | Recommended usable budget | App partition headroom |
| --- | ---: | ---: | ---: | ---: |
| ESP32-C3, 4 MB | 2,031,616 | 1,978,368 | 1,970,176 | 347,020 B |
| ESP32-S3, 16 MB deployment model | 14,548,992 | 14,389,248 | 14,381,056 | 416,685 B |

Average-density estimates from `benchmarks/flash_budget.json` put the full
corpus at 847 articles in the C3 budget or 6,183 in the S3 budget for the
pre-optimization v1 gzip-9 source-order baseline. Independent Zstd 19 reaches
862 or 6,294 articles (+15/+111), while a 64 KiB pack dictionary reaches 1,097
or 8,009 (+250/+1,826). These are average-density and partial-pack estimates,
not a claim that a particular article selection has been built or installed.

## Measured versus hardware-dependent

Measured and reproducible in this worktree:

- the 663-case host matrix and implementation fingerprints;
- PWPK host validation, browser server routes, and live Chromium fixtures;
- C3 serial heap, decoder-allocation, storage, and board telemetry;
- clean C3 and S3 PlatformIO builds;
- generated flash/resource budgets and embedded browser asset sizes;
- C3 post-flash idle stability with the 16 KiB-capable dynamic TLS
  configuration;
- a complete 343,943-byte C3 station-mode web install, including checksum,
  pack validation, storage accounting, BLE suspension, and BLE recovery.

Still hardware-dependent or unavailable here:

- S3 heap, PSRAM, decoder, Wi-Fi, and storage timings; no S3 board was
  connected;
- validation of the repository's 16 MB S3 deployment model on the actual
  module, because the current PlatformIO board definition reports an 8 MB,
  no-PSRAM DevKitC-1;
- multi-client HTTP throughput and latency, because the available station link
  was too slow for a representative measurement;
- representative web-GUI install throughput and concurrency; the successful
  install above is a completion smoke test without a duration claim;
- a production-quality ESP-side PWPK decoder design for C3.

## Verification completed

The final host suite passed with `103 passed`. Both firmware targets built
successfully:

```text
python3 -m pytest -q                         103 passed
pio run -d firmware -e esp32-c3              success
pio run -d firmware -e esp32-s3              success
```

No production archive or source database was replaced by these measurements;
generated firmware, fixture packs, and local build outputs remain outside the
tracked production inputs as required by the repository guide.
