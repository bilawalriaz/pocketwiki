# Browser serving benchmark

The browser path is an experimental PWPK route, separate from the production
`content.bin`/`index.bin` reader. `tools/pwpk_browser_server.py` models the
device contract: look up an article, read only its compressed byte range, and
let the client decode it. `firmware/main/pwpk_http.c` exposes the corresponding
device routes under `/pwpk/<name>/...`; the embedded reader is served at
`/pwpk-ui/` when `CONFIG_POCKETWIKI_PWPK_SERVICE=y`.

## Reproduction

```sh
python3 tools/benchmark_browser.py \
  --db /path/to/wiki.sqlite --output build/browser-corpus
python3 tools/pwpk_browser_server.py build/browser-corpus/*.pwpk --port 8765
```

Open `/benchmark` in a Chromium browser. The page verifies article SHA-256
values, dictionary reuse, and the optional native `Content-Encoding: zstd`
path. The same server supports `Range: bytes=a-b` for a raw-flash-backed
implementation. Host route coverage is in
`tests/test_pwpk_browser_server.py`; the C route and reader are compiled by
both firmware builds and the host firmware archive tests.

## Measured result (10 September 2026)

The current Chromium run is recorded in `benchmarks/browser_results.json`:

| Variant | Requests | Successes | Dictionary fetches | Median decode | P95 decode |
| --- | ---: | ---: | ---: | ---: | ---: |
| independent Zstd | 50 | 50 | 0 | 0.1 ms | 0.2 ms |
| 64 KiB blocks | 50 | 50 | 0 | 0.2 ms | 2.1 ms |
| 16 KiB dictionary | 50 | 50 | 1 | 0.1 ms | 0.2 ms |

Native Zstd `Content-Encoding` also succeeded for an independent frame in this
run (`5898` decoded bytes, response header `Content-Encoding: zstd`). This is
browser-version evidence, not a universal compatibility guarantee; retain the
WASM fallback and feature-detect native support.

The shipped fallback assets are from `@bokuweb/zstd-wasm` 0.0.27. The measured
asset set is 278,877 bytes raw and 89,701 bytes when each file is gzip-compressed
for transport. The 251,806-byte WASM module is the dominant asset. These are
transfer measurements; the actual firmware image size is the authoritative
on-flash cost.

Dictionary frames work through the WASM adapter, which accepts
`(frame, dictionary)`. The native Web Streams API has no dictionary parameter,
so dictionary packs must use the WASM path. The device streams the frame and
fetches the dictionary once per pack/session; it does not buffer a whole pack.

## Boundaries

The browser benchmark does not measure ESP Wi-Fi throughput, heap, or a real
S3 board. The live C3 serial run found too little contiguous heap for an ESP
Zstd context with normal services enabled, making browser-side decoding the
preferred C3 PWPK mode. See [DEVICE_BENCHMARK.md](DEVICE_BENCHMARK.md) and
[FINAL_RECOMMENDATION.md](FINAL_RECOMMENDATION.md).
