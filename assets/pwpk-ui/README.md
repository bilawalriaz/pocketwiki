# PWPK browser prototype

`tools/pwpk_browser_server.py` exposes PWPK frames as bounded, range-readable
HTTP responses. It accepts one or more unpacked `content.bin`/`index.bin`
directories or portable `.pwp` files, so installed-pack selection and the
wire format can be tested without an ESP. Start it with:

```sh
python tools/pwpk_browser_server.py build/content other.pwp --port 8080
```

Open `/` in a browser. The page fetches only the compressed article frame and
reports whether the browser can decode it with `DecompressionStream("zstd")`.
Dictionary bytes are fetched once at `/api/packs/<pack>/dictionary`; the page
passes them to an optional `window.pwpkWasmDecode(frame, dictionary)` adapter.

`device.html` and everything under `vendor/` are the assets the device serves
at `/pwpk-ui/`: `tools/embed_pwpk_browser.py` gzips each file into a C array
and registers the route when `CONFIG_POCKETWIKI_PWPK_SERVICE` is enabled. The
vendored `zstd.js` and `zstd.wasm` come from the npm packages pinned in
`package.json`; both are BSD-3-Clause, and their notices are kept in
`vendor/NOTICE` and `vendor/ZSTD-LICENSE`. Regenerate them with `npm install`
in this directory and copy the files you need into `vendor/`.

`tools/benchmark_browser_assets.py` measures the gzipped size of every shipped
file and writes `benchmarks/browser_assets.json`.
