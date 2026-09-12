# Pack catalogue

The public catalogue is `https://packs.educated.space/index.json`. It lists
immutable `.pwp` files served by the PocketWiki Cloudflare Worker.

The Android app has a bundled fallback catalogue. It refreshes the public
catalogue when the phone has internet access. The `/manage` page can ask the
device to refresh its own cached catalogue through station Wi-Fi.

Before installation, Android checks the declared byte count and SHA-256. The
device still performs its own size, CRC, header, and index checks.

## Publish a catalogue update

Article text must satisfy its invariants before a publish, and changing an
article makes every pack containing it a different archive. So the order is:

1. Check the content: `python3 tools/finalize_articles.py --check` reports any
   drift, and `--apply` repairs it. Article text lives in the `pocketwiki-content`
   checkout; see the development guide for the invariants.
2. Edit `packs/catalog-source.json` there. Keep each pack `id` stable, and bump a
   pack's `version` whenever its content changes. Published pack objects are
   immutable at their URL, so reusing a version silently swaps the bytes behind a
   cached URL. Bump `catalog_version` as well: it names the directory the packs
   are published under.
3. Authenticate Wrangler with the correct Cloudflare account.
4. Run the publisher.

```sh
python3 tools/publish_pack_catalog.py
```

The publisher builds and checks every archive, updates Android's fallback
catalogue, uploads immutable pack objects, and uploads `index.json` last. This
last upload makes the new release visible as one catalogue update.

The firmware embeds `android/app/src/main/assets/pack_catalog.json`, so rebuild
and reflash after a publish when the embedded catalogue should match the live one.

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
after the export. Every article is written through
`tools/article_text.py:finalize_article`, so it carries its CC BY-SA footer and
carries no generator commentary.
