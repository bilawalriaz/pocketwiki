---
name: pocketwiki-pack-authoring
description: Create, curate, validate, and publish PocketWiki article packs and catalogue updates while preserving the existing .pwp format, device constraints, metadata conventions, and install workflow.
---

# PocketWiki pack authoring

Use this skill when adding a pack, refreshing a pack's articles, changing the
public catalogue, or preparing packs for installation on an ESP32 board.

## Read first

From the repository root, read `AGENTS.md`, `README.md`,
`docs/DEVELOPMENT.md`, `docs/PACK_CATALOG.md`, and `docs/ARCHIVE_FORMAT.md`.
Read `docs/HARDWARE.md` before changing firmware, partitions, or anything that
affects the C3 and S3 storage limits.

Article text and `packs/catalog-source.json` live in the companion
`pocketwiki-content` checkout, which `tools/content_paths.py` resolves. Inspect
the existing directories under `articles/` there before choosing a structure.
Existing packs are the compatibility reference: keep their catalogue fields and
their reading style.

## Pack and catalogue conventions

- Keep pack ids stable, lowercase, and limited to the existing safe hyphenated
  form, for example `space-and-the-cosmos`.
- Keep user-facing catalogue `name` values in title case. Do not expose storage
  ids as display labels.
- Keep descriptions concise and factual. Do not claim that a pack replaces a
  school programme, professional advice, or an official reference.
- Keep the catalogue shape established by `tools/build_pack_catalog.py`: `id`,
  `name`, `version`, `description`, `articles`, `bytes`, `sha256`, `url`, and
  the pack-level `license` field.
- Keep the licence metadata at pack level and do not change licence terms as
  part of ordinary pack work. Every article keeps the CC BY-SA footer it was
  published with; see `ATTRIBUTION.md` in the content repository.
- Use immutable pack URLs. Increase a pack's `version` when its article set or
  article content changes, and `catalog_version` when publishing a new
  catalogue pointer. Never replace an old version under a different meaning.
- Keep each generated `.pwp` below the enforced 2 MiB install limit, and design
  packs so they fit the target device's optional-pack storage.

## Creating content

Prefer the existing curated sources under the content repository's `articles/`
directories. A new source has to be adapted into the same shape: one correct
leading `# Title`, deterministic output for the same input, a per-article
CC BY-SA attribution footer naming the creator and linking the source, and no
generator commentary left in the body.

Keep the selection and copying logic in a small reproducible tool rather than
maintaining a large list of copied files by hand. Validate that every selected
source file exists, and remove stale generated files from the pack directory.
A normal firmware build must never fetch network content.

## Build, publish, and embed

From the repo root:

```sh
python3 tools/build_pack_catalog.py
python3 -m pytest -q
python3 tools/publish_pack_catalog.py
python3 tools/embed_catalog.py
```

Run the publisher only when the catalogue is meant to be hosted or updated. It
uploads the immutable pack objects first and `index.json` last, so a reader
never sees a catalogue that points at packs which are not there yet. Use
`--deploy-worker` only when the Worker or its configuration changed.

After changing firmware, catalogue assets, partition files, or embedded web
assets, build both targets:

```sh
pio run -d firmware -e esp32-c3
pio run -d firmware -e esp32-s3
```

If an attached board is in scope, identify its chip and port before flashing.
Use `tools/flash_all.py` with the matching staged directory. Do not erase the
optional-pack filesystem unless the user asks for that.

## Release verification

Check that the generated catalogue has the intended version and pack count,
that every pack id is unique, that names are properly cased, and that no
unexpected metadata fields were introduced. For a hosted release, fetch
`https://packs.educated.space/index.json` with a normal User-Agent, then fetch
every listed pack and verify HTTP success, byte count, SHA-256, archive
readability, and article count.

For the device homepage's `Your libraries` labels, check both paths: the
catalogue lookup must return the human-facing name, and the firmware fallback
must title-case hyphen and underscore ids. The fallback is recovery behaviour,
so it must not be the normal way new names arrive.

Do not commit generated firmware, `.pwp` archives, downloaded corpora,
`sdkconfig`, managed components, or local build directories. Commit source
articles, catalogue source, reproducible tools, embedded assets the build
needs, and this skill when those are the intended changes.
