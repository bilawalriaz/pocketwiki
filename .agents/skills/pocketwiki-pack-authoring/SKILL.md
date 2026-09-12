---
name: pocketwiki-pack-authoring
description: Create, curate, validate, and publish PocketWiki article packs and catalogue updates while preserving the existing .pwp format, device constraints, metadata conventions, and install workflow.
---

# PocketWiki Pack Authoring

Use this skill when adding a new PocketWiki pack, refreshing pack articles, changing the public catalogue, or preparing packs for installation on an ESP32 device.

## Read first

From the repository root, read `AGENTS.md`, `README.md`, `docs/DEVELOPMENT.md`, and `docs/ARCHIVE_FORMAT.md`. Read `docs/HARDWARE.md` before changing firmware, partitions, or anything that affects the C3/S3 storage limits.

Inspect `packs/catalog-source.json` and the existing directories under `articles/db-packs/` in the `pocketwiki-content` checkout before choosing a structure. Existing packs are the compatibility reference: preserve their catalogue fields and their clean article-reading style.

## Pack and catalogue conventions

- Keep pack IDs stable, lowercase, and limited to the existing safe hyphenated form, for example `space-and-the-cosmos`.
- Keep user-facing catalogue `name` values properly cased in title case. Do not expose storage IDs as display labels.
- Keep descriptions concise and factual. Avoid claiming that a pack replaces a school programme, professional advice, or an official reference.
- Keep the catalogue shape established by `tools/build_pack_catalog.py`: `id`, `name`, `version`, `description`, `articles`, `bytes`, `sha256`, `url`, and the existing pack-level `license` field.
- Do not add per-article `Source: Wikipedia` footer text or ad-hoc catalogue provenance fields. Keep the current pack-level licensing metadata and do not alter licensing terms as part of ordinary pack creation.
- Use immutable pack URLs. Increase a pack's `version` when its article set or article content changes; increase `catalog_version` when publishing a new catalogue pointer. Never silently replace an old version under a different meaning.
- Keep each generated `.pwp` below the repository's enforced 2 MiB install limit and design packs so they fit the target device's optional-pack storage.

## Creating content

Prefer existing curated article sources under `articles/db-packs/` in the `pocketwiki-content` checkout. For a new source database, read it read-only, select only completed/quality-gated content, normalize each article to one correct leading `# Title`, and keep generated files deterministic. Local Ollama/LM Studio may be used for approved local embeddings or drafting, but a normal firmware build must not fetch network content.

For repeated curation, put the selection and copying logic in a small reproducible tool under `tools/` rather than manually maintaining a large list of copied files. Validate every selected source file exists and remove stale generated files from the new pack directory.

## Build, publish, and embed

From the repo root:

```sh
python3 tools/build_pack_catalog.py
python3 -m pytest -q
python3 tools/publish_pack_catalog.py
python3 tools/embed_catalog.py
```

Run the publisher only when the user has asked for the catalogue to be hosted or updated. It uploads immutable pack objects first and `index.json` last. Use `--deploy-worker` only when the Worker/configuration changed.

After changing firmware, catalogue assets, partition files, or embedded web assets, build both targets:

```sh
pio run -d firmware -e esp32-c3
pio run -d firmware -e esp32-s3
```

If an attached board is explicitly in scope, identify its chip and port before flashing. Use `tools/flash_all.py` with the matching staged directory; do not erase the optional-pack filesystem unless the user asks for that.

## Release verification

Verify the generated catalogue has the intended version and pack count, every pack has a unique ID, all names are properly cased, and no unexpected metadata fields were introduced. For a hosted release, fetch `https://packs.educated.space/index.json` with a normal User-Agent, then fetch every listed pack and verify HTTP success, byte count, SHA-256, archive readability, and article count. Check that article bodies do not contain an unwanted trailing `Source: https://en.wikipedia.org` line.

For the device homepage's `Your libraries` labels, verify both paths: catalogue lookup must return the human-facing name, and the firmware fallback must title-case hyphen/underscore IDs. The fallback is recovery behavior; it must not be the normal way new names are supplied.

Do not commit generated firmware, `.pwp` archives, downloaded corpora, `sdkconfig`, managed components, or local build directories. Commit source articles, catalogue source, reproducible tools, embedded assets required by the build, and this skill when those are the intended repository changes.
