# Pack catalogue

The public catalogue is `https://packs.educated.space/index.json`. It lists
immutable `.pwp` files served by the PocketWiki Cloudflare Worker.

The catalogue is a release, not the whole shelf pool: 50 clustered shelves plus
the built-in guide and the four UK curriculum packs, published as catalogue
version 10 (12.2 MB of packs, 5,452 articles). The whole release fits the 16 MB
board's 14 MB pack store; a 4 MB board holds about seven packs of it.

Each entry carries a `collection`, and the document carries a `collections`
array of `{id, name, description}` records, so a client can group what it is
offered instead of listing 55 packs flat.

The Android app has a bundled fallback catalogue. It refreshes the public
catalogue when the phone has internet access. The `/manage` page can ask the
device to refresh its own cached catalogue through station Wi-Fi.

Before installation, Android checks the declared byte count and SHA-256. The
device still performs its own size, CRC, header, and index checks.

## Publish a catalogue update

1. Edit `packs/catalog-source.json` in the content checkout, or
   `packs/catalogue-selection.json` to change which shelves ship.
2. Keep each pack `id` stable.
3. Increase a pack's `version` when its content changes, and the catalogue's
   `catalog_version` when the release pointer moves.
4. Authenticate Wrangler with the correct Cloudflare account.
5. Run the publisher.

```sh
python3 tools/publish_pack_catalog.py
```

The publisher builds and checks every archive, updates Android's fallback
catalogue, uploads the immutable pack objects, and uploads `index.json` last.
That final upload is what makes the new release visible, so a reader never sees
a catalogue that points at packs which are not there yet.

Use `--deploy-worker` only when the Worker code or configuration also changed.
Run `python3 -m pytest -q` after you change the catalogue.

The device reads the published document with `firmware/main/catalog_index.c`,
which streams it and keeps one pack entry at a time: there is no limit on how
many packs a release may carry, but each id and name must be shorter than 48
bytes and each url shorter than 96. `tests/test_pack_catalog.py` reads a built
catalogue back through that code, so a release the device could not read fails
the suite instead of the board.

## Curated shelves

`articles/db-packs/` holds the hand-built shelves. The UK curriculum packs are
published from `db-packs/uk-curriculum/`. The other themed shelves are not in
the catalogue, and their sources stay in the content repository so a release
can bring any of them back by adding the entry to `packs/catalog-source.json`.

Each entry gives a pack its id, name, version, description, the directory its
articles are read from, and the ordered article list. Every article must keep
its CC BY-SA attribution footer.

## Clustered shelves

`articles/cluster-packs/` holds the shelf pool: 234 shelves chosen by meaning
rather than by title terms, generated from the corpus embeddings. Each shelf is
one Markdown directory, and the whole pool is written deterministically, so a
re-run over unchanged input writes identical files.

- A shelf holds about 100 articles. A cluster larger than 150 articles is
  bisected until it fits; one below 40 merges into its nearest neighbour.
  Shelves are then grouped into collections of about twelve shelves.
- `articles/cluster-packs/manifest.json` records the collections, every shelf,
  its article ids, and the eight articles nearest its centroid. Those anchors
  identify a shelf between runs: a shelf keeps its id while it still shares four
  of them with the previous run, and its version only moves when its article
  list does.
- `packs/catalogue-selection.json` lists the shelves a release publishes, in
  catalogue order. The rest stay in the pool, so narrowing or widening the
  catalogue is an edit to that file.
- `packs/cluster-labels.json` holds the curated names and descriptions. Shelf
  entries are keyed by pack id and may carry `"id"` to rename a shelf; collection
  entries are keyed by one of the shelves they hold and may carry `"id"` to give
  the collection its published name.

## Browse the catalogue

```sh
python3 tools/build_pack_browser.py
```

writes `packs.html` into the content checkout: one self-contained page that
opens from `file://` with no network and no server. It embeds the catalogue, the
collections, and every article title, so packs can be searched, filtered by
collection or kind, and sorted by name, article count, or install size. Each
install size is measured by building the pack, so regenerating the page also
checks every pack against the upload limit. `packs.html` is generated: change
the generator, not the page.

## The device's embedded fallback

`tools/embed_catalog.py` embeds the published catalogue in the firmware, which
is what the `/manage` page falls back to when the browser cannot reach
`packs.educated.space`. The one it embeds is
`android/app/src/main/assets/pack_catalog.json`, the exact bytes the publisher
uploaded, because the device verifies downloaded pack sizes and SHA-256s against
that list. Those values have to match the packs actually served, so the embedded
copy is a snapshot of the live catalogue rather than a rebuild from
`catalog-source.json`.

At 55 packs the array is 28 KB, 13 KB more than the 31-pack catalogue it
replaces, and it lands in the 1.5 MB application partition. The C3 build sits at
78% of that partition, so a much larger release needs the fallback trimmed or
moved into the pack store.
