import json

import pytest

from content_paths import ARTICLES, CATALOG, available
from tools import archive_format as af
from tools.build_pack_catalog import build_catalog

# These tests read the real article corpus and pack manifest, which live in the
# separate pocketwiki-content repository.
pytestmark = pytest.mark.skipif(
    not available(),
    reason="pocketwiki-content checkout not present; set POCKETWIKI_CONTENT_DIR",
)


def test_example_catalog_builds_valid_versioned_packs(tmp_path):
    catalog = build_catalog(CATALOG, tmp_path)
    source = json.loads(CATALOG.read_text())
    assert catalog["schema"] == 1
    assert len(catalog["packs"]) == len(source["packs"])
    assert len({pack["id"] for pack in catalog["packs"]}) == len(catalog["packs"])
    for pack in catalog["packs"]:
        path = tmp_path / f"v{catalog['catalog_version']}" / pack["url"].rsplit("/", 1)[-1]
        data = path.read_bytes()
        content, index = af.unpack_pack_file(data)
        archive = af.Archive(content, index)
        assert archive.format_version == af.FORMAT_VERSION_LEGACY
        assert archive.count == pack["articles"]
        assert len(data) == pack["bytes"]


def test_catalog_source_uses_existing_unique_articles():
    source = json.loads(CATALOG.read_text())
    for pack in source["packs"]:
        assert len(pack["articles"]) == len(set(pack["articles"]))
        articles_dir = pack.get("dir", "starter")
        assert all((ARTICLES / articles_dir / name).is_file()
                   for name in pack["articles"])
