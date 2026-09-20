"""Checks over the published pack catalogue.

The catalogue and the article text it names live in the pocketwiki-content
checkout, so this file skips when it is not there. CI clones it before running
the suite; see tools/content_paths.py for how the path is resolved.
"""

import json

import pytest

from tools import archive_format as af
from tools import content_paths
from tools.build_pack_catalog import build_catalog

pytestmark = pytest.mark.skipif(
    not content_paths.available(),
    reason=f"no pocketwiki-content checkout at {content_paths.CONTENT}",
)


def catalog_source() -> dict:
    return json.loads(content_paths.CATALOG.read_text(encoding="utf-8"))


def test_example_catalog_builds_valid_versioned_packs(tmp_path):
    catalog = build_catalog(content_paths.CATALOG, tmp_path)
    source = catalog_source()
    assert catalog["schema"] == 1
    assert len(catalog["packs"]) == len(source["packs"])
    assert len({pack["id"] for pack in catalog["packs"]}) == len(catalog["packs"])
    for pack in catalog["packs"]:
        path = tmp_path / f"v{catalog['catalog_version']}" / pack["url"].rsplit("/", 1)[-1]
        data = path.read_bytes()
        content, index = af.unpack_pack_file(data)
        archive = af.Archive(content, index)
        assert archive.format_version == af.FORMAT_VERSION
        assert archive.dict_len > 0
        assert archive.count == pack["articles"]
        assert len(data) == pack["bytes"]


def test_published_catalogue_is_readable_by_the_device(tmp_path, catalog_harness):
    """What the publisher writes is what the firmware's reader can use.

    The device reads the published index.json with catalog_index.c, one pack at a
    time and inside fixed field caps, so this reads a real release back through
    that code: every pack resolves by url, the built-in included, and every name
    is found in one pass.
    """
    import subprocess

    catalog = build_catalog(content_paths.CATALOG, tmp_path)
    index = (tmp_path / "index.json").read_bytes()

    def harness(*args):
        return subprocess.run([catalog_harness, "256", *args], input=index,
                              capture_output=True, check=True).stdout.decode()

    # Every pack is inside the caps the device reads with, so no entry can be
    # rejected on the device while the publisher accepts it.
    for pack in catalog["packs"]:
        assert len(pack["id"]) < 48, pack["id"]
        assert len(pack["name"]) < 48, pack["name"]
        assert len(pack["url"]) < 96, pack["url"]
        assert len(pack["sha256"]) == 64

    names = harness("names", *[pack["id"] for pack in catalog["packs"]])
    filled = int(names.splitlines()[0].split("=")[1])
    assert filled == len(catalog["packs"])
    assert [line.split("|")[1] for line in names.splitlines()[1:]] == \
        [pack["name"] for pack in catalog["packs"]]

    for pack in (catalog["packs"][0], catalog["packs"][-1]):
        fields = harness("find", "", pack["url"]).splitlines()
        assert fields[0] == "ok=1"
        found = fields[1].split("|")
        assert found[0] == pack["id"]
        assert found[3] == pack["sha256"]
        assert int(found[4]) == pack["bytes"]


def test_catalog_source_uses_existing_unique_articles():
    for pack in catalog_source()["packs"]:
        assert len(pack["articles"]) == len(set(pack["articles"]))
        directory = content_paths.article_dir(pack.get("dir", "starter"))
        missing = [name for name in pack["articles"] if not (directory / name).is_file()]
        assert not missing, f"{pack['id']} names missing articles: {missing[:3]}"


def test_clustered_shelves_match_the_manifest_and_are_named():
    """The catalogue, the shelf manifest, the selection, and the labels agree.

    The pool in articles/cluster-packs/ is larger than the release: the
    catalogue publishes exactly the shelves packs/catalogue-selection.json names,
    with the manifest's own name and article list, inside a published collection.
    """
    source = catalog_source()
    manifest = json.loads(content_paths.SHELF_MANIFEST.read_text(encoding="utf-8"))
    labels = json.loads(content_paths.LABELS.read_text(encoding="utf-8"))
    selection = json.loads(content_paths.SELECTION.read_text(encoding="utf-8"))

    shelves = {pack["id"]: pack for pack in manifest["packs"]}
    catalogued = {pack["id"]: pack for pack in source["packs"]
                  if pack.get("dir", "").startswith("cluster-packs/")}
    assert catalogued, "the catalogue holds no clustered shelves"
    assert set(catalogued) == set(selection["shelves"]), \
        "the catalogue and the selection list disagree"
    assert set(catalogued) <= set(shelves), "a published shelf is not in the pool"

    named = dict(labels["packs"])
    for key, entry in labels["packs"].items():
        if entry.get("id"):
            named[entry["id"]] = entry
    for pack_id, shelf in shelves.items():
        assert len(shelf["articles_list"]) == shelf["articles"] == len(shelf["article_ids"])
        assert pack_id in named, f"{pack_id} has no curated label"
    for pack_id, entry in catalogued.items():
        shelf = shelves[pack_id]
        assert entry["articles"] == shelf["articles_list"]
        assert entry["name"] == shelf["name"]
        assert entry["collection"] == shelf["collection"]

    # Every published collection is described for clients, and every shelf it
    # claims is published under it.
    described = {collection["id"]: collection for collection in source["collections"]}
    for collection in source["collections"]:
        assert collection["name"] and collection["description"]
    for pack_id, entry in catalogued.items():
        assert entry["collection"] in described, entry["collection"]
    published = {entry["collection"] for entry in catalogued.values()}
    assert set(described) == published, "a published collection has no shelves"

    collection_ids = {collection["id"] for collection in manifest["collections"]}
    assert collection_ids == {shelf["collection"] for shelf in shelves.values()}
    for collection in manifest["collections"]:
        keys = {collection["id"], *collection["packs"]}
        assert keys & set(labels["collections"]), f"{collection['id']} has no curated label"
