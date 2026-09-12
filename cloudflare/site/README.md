# PocketWiki landing deployment

This Worker serves the PocketWiki landing page at
`https://educated.space/pocketwiki` and owns only the `/pocketwiki*` route; it
does not replace the existing Educated homepage.

## The page

[`pocketwiki3.html`](../../pocketwiki3.html) is the landing page: the project
overview rebuilt on [BoardUI](https://www.boardui.com/components) — its semantic
tokens, composite type scale, and component geometry (button, chip, input,
switch, radio-card, notification, tabs, table, carousel, theme-toggle) ported to
plain CSS so the page stays one self-contained file. The content, section order,
flasher and catalogue script are unchanged.

`prepare.sh` stages it at both `public/pocketwiki/` (the canonical URL) and
`public/pocketwiki3/`, so the canonical and `og:url` tags point at
`/pocketwiki`. [`index.html`](../../index.html) is the earlier design and is
staged at `/pocketwiki2/` only.

Two rules hold the edition together:

- The accent ramp is re-tinted to the PocketWiki hue (`--color-accent-*`), which
  is how BoardUI expects a brand to diverge from its defaults.
- The catalogue marquee is untouched. Its stylesheet is copied verbatim from
  `index.html` and the tokens it reads (`--border`, `--text`, `--accent`, …) are
  re-declared on `.pack-marquee-stack` at their original values, so the cards
  render exactly as they do on `/pocketwiki2`.

## Deploy

The page flashes boards itself. Its browser utility reads the verified firmware
manifest from `https://packs.educated.space/firmware/index.json`, while the site
deployment contains the page and screenshots:

```sh
pio run -d firmware -e esp32-c3
pio run -d firmware -e esp32-s3
python3 tools/publish_firmware.py
sh cloudflare/site/prepare.sh
wrangler deploy --config cloudflare/site/wrangler.jsonc
```

`prepare.sh` copies the page and screenshots, then stages a local preview copy
of `firmware/dist/<env>/`. Publish the actual browser-flasher images with
`python3 tools/publish_firmware.py`; it exits non-zero when a build is missing
or an image overflows its partition, so R2 cannot receive a flash plan that
would fail on the board. `python3 tools/stage_web_firmware.py --check` validates
without writing.

The pack definitions in
[`packs/catalog-source.json`](https://github.com/bilawalriaz/pocketwiki-content/blob/main/packs/catalog-source.json), the landing
catalogue builder, the images in
[`assets/screenshots/`](../../assets/screenshots), and the partition tables in
[`firmware/`](../../firmware) remain the sources of truth. `prepare.sh` generates
the landing page's article-title catalogue from those pack definitions. The
`public/` directory is ignored because it is a deployment staging copy.
