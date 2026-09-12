# Pack catalogue

The public catalogue is `https://packs.educated.space/index.json`. It lists
immutable `.pwp` files served by the PocketWiki Cloudflare Worker.

The Android app has a bundled fallback catalogue. It refreshes the public
catalogue when the phone has internet access. The `/manage` page can ask the
device to refresh its own cached catalogue through station Wi-Fi.

Before installation, Android checks the declared byte count and SHA-256. The
device still performs its own size, CRC, header, and index checks.

## Publish a catalogue update

1. Edit `packs/catalog-source.json` in the `pocketwiki-content` checkout.
2. Keep each pack `id` stable.
3. Increase the pack `version` when its content changes.
4. Authenticate Wrangler with the correct Cloudflare account.
5. Run the publisher.

```sh
python3 tools/publish_pack_catalog.py
```

The publisher builds and checks every archive, updates Android's fallback
catalogue, uploads immutable pack objects, and uploads `index.json` last. This
last upload makes the new release visible as one catalogue update.

Use `--deploy-worker` only when the Worker code or configuration also changed.
Run `python3 -m pytest -q` after you change the catalogue.

## Rebuild source packs

The database-backed themed pack directories are under `articles/db-packs/` in the
`pocketwiki-content` checkout.
`tools/build_db_packs.py` reads a local wiki-distill
`educational-source.db` read-only, selects completed quality-gated
distillations, and writes deterministic Markdown source articles. For example:

```sh
python3 tools/build_db_packs.py \
  --db ~/wiki-distill/educational-source.db \
  --articles-per-pack 100 --catalog-version 7
```

The generator preserves unrelated hand-curated catalogue entries and bumps a
changed pack's version so published URLs remain immutable. Run the publisher
after the export. Each article must retain its source and CC BY-SA attribution.
