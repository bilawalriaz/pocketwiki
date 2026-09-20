"""Host checks for the firmware's streaming catalogue reader.

The device cannot hold the catalogue text plus a parsed tree, and it cannot hold
an entry per pack either, so catalog_index.c answers one question per pass: find
one pack by id or url, or name several packs at once. A scanner like that is
only as good as its edge cases, so these checks run it against a real catalogue
shape, token-boundary sizes, a catalogue far larger than any index would have
held, and documents that must be rejected: a wrong schema, missing fields, an
over-long value, and truncation.
"""

import hashlib
import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_MAIN = ROOT / "firmware" / "main"
# The catalogue the device ships with. It is a real published index.json, so
# the field shapes and value lengths exercised here are the ones the reader has
# to accept.
SOURCE_CATALOG = (ROOT / "android" / "app" / "src" / "main" / "assets"
                  / "pack_catalog.json")


def sha_of(name: str) -> str:
    return hashlib.sha256(name.encode()).hexdigest()


def live_catalog(pack_count: int = 30, **overrides) -> dict:
    """A catalogue shaped like the published index.json, from the shipped copy."""
    source = json.loads(SOURCE_CATALOG.read_text(encoding="utf-8"))
    packs = [dict(entry) for entry in source["packs"][:pack_count]]
    document = {
        "schema": 1,
        "catalog_version": 7,
        "generated_at": "2026-09-17T10:00:00Z",
        "packs": packs,
    }
    document.update(overrides)
    return document


def synthetic_catalog(pack_count: int) -> dict:
    """A catalogue with no relationship to the repository's shelves."""
    packs = [{
        "id": f"pack-{index:03d}",
        "name": f"Pack {index:03d}",
        "version": 1,
        "description": f"Shelf number {index}.",
        "articles": 100 + index,
        "bytes": 200000 + index,
        "url": f"https://packs.example/v9/pack-{index:03d}-v1.pwp",
        "sha256": sha_of(f"pack-{index:03d}"),
    } for index in range(pack_count)]
    return {"schema": 1, "catalog_version": 9, "generated_at": "2026-09-17T10:00:00Z",
            "packs": packs}


@pytest.fixture(scope="module")
def harness(catalog_harness):
    return Path(catalog_harness)


def run(harness: Path, document, chunk: int = 256, *args):
    """Run one question against a document; returns the printed fields and lines."""
    payload = document if isinstance(document, str) else json.dumps(document)
    result = subprocess.run([str(harness), str(chunk), *[str(a) for a in args]],
                            input=payload.encode(), capture_output=True, check=True)
    lines = result.stdout.decode().splitlines()
    fields = dict(field.split("=", 1) for field in lines[0].split(" ") if "=" in field)
    return fields, lines[1:]


def find(harness: Path, document, pack_id: str = "", url: str = "", chunk: int = 256):
    fields, lines = run(harness, document, chunk, "find", pack_id, url)
    if fields["ok"] != "1":
        return None
    return lines[0].split("|")


@pytest.mark.parametrize("chunk", [256, 37, 1])
def test_every_pack_resolves_at_every_token_boundary(harness, chunk):
    document = live_catalog()
    for position in (0, len(document["packs"]) // 2, len(document["packs"]) - 1):
        pack = document["packs"][position]
        found = find(harness, document, pack_id=pack["id"], chunk=chunk)
        assert found is not None, pack["id"]
        assert found[0] == pack["id"]
        assert found[1] == pack["name"]
        assert found[2] == pack["url"]
        assert found[3] == pack["sha256"]
        assert int(found[4]) == pack["bytes"]
        assert int(found[5]) == pack["version"]
        assert int(found[6]) == pack["articles"]


@pytest.mark.parametrize("chunk", [256, 37, 1])
def test_header_reports_schema_version_and_timestamp(harness, chunk):
    fields, _ = run(harness, live_catalog(), chunk, "header")
    assert fields["ok"] == "1"
    assert int(fields["version"]) == 7
    assert fields["at"] == "2026-09-17T10:00:00Z"


def test_find_resolves_id_and_url_and_misses_cleanly(harness):
    document = live_catalog()
    first = document["packs"][0]
    assert find(harness, document, pack_id=first["id"])[0] == first["id"]
    assert find(harness, document, url=first["url"])[0] == first["id"]
    assert find(harness, document, pack_id="no-such-pack") is None
    assert find(harness, document, url="https://example.invalid/x.pwp") is None


def test_names_fill_every_requested_pack_in_one_pass(harness):
    document = synthetic_catalog(120)
    wanted = [f"pack-{index:03d}" for index in (0, 1, 57, 119, 42)]
    fields, lines = run(harness, document, 256, "names", *wanted)
    assert int(fields["filled"]) == len(wanted)
    assert [line[2:].split("|")[1] for line in lines] == [f"Pack {index:03d}" for index in (0, 1, 57, 119, 42)]


def test_names_leave_unknown_packs_untouched(harness):
    document = live_catalog()
    fields, lines = run(harness, document, 256, "names", "no-such-pack", document["packs"][0]["id"])
    assert int(fields["filled"]) == 1
    assert lines[0][2:].split("|")[1] == ""
    assert lines[1][2:].split("|")[1] == document["packs"][0]["name"]


def test_a_catalogue_far_larger_than_any_index_still_resolves(harness):
    """The reader has no entry cap: a release may carry as many packs as it likes."""
    document = synthetic_catalog(400)
    last = document["packs"][-1]
    found = find(harness, document, pack_id=last["id"])
    assert found is not None and found[0] == last["id"] and int(found[4]) == last["bytes"]
    fields, _ = run(harness, document, 256, "validate")
    assert fields["ok"] == "1"


def test_collection_metadata_does_not_disturb_pack_reading(harness):
    """The published catalogue carries a top-level collections array."""
    document = live_catalog()
    document["collections"] = [
        {"id": "pure-mathematics", "name": "Pure Mathematics", "description": "Algebra and analysis."},
        {"id": "physics", "name": "Physics", "description": "Matter, motion and fields."},
    ]
    document["packs"][1]["collection"] = "physics"
    found = find(harness, document, pack_id=document["packs"][1]["id"])
    assert found is not None and found[0] == document["packs"][1]["id"]
    fields, _ = run(harness, document, 256, "validate")
    assert fields["ok"] == "1"


def test_descriptions_with_json_syntax_do_not_confuse_the_scanner(harness):
    document = live_catalog()
    document["packs"][0]["description"] = 'tricky: {"packs": [{"url": "https://evil/x"}], "id": "fake"} \\ "quoted"'
    document["packs"][1]["extra"] = {"nested": {"a": [1, 2, {"b": "packs: ["}]}}
    assert find(harness, document, pack_id=document["packs"][0]["id"])[0] == document["packs"][0]["id"]
    assert find(harness, document, pack_id=document["packs"][1]["id"])[0] == document["packs"][1]["id"]


def test_escaped_characters_survive(harness):
    document = live_catalog()
    document["packs"][0]["name"] = 'Quote" and newline\n end'
    assert find(harness, document, pack_id=document["packs"][0]["id"])[1] == 'Quote" and newline\\n end'


def test_sorted_keys_read_like_the_published_catalogue(harness):
    """The publisher sorts keys, which puts `schema` after `packs`.

    A reader that decides before the last byte would reject every published
    release, so this runs the document exactly as `build_pack_catalog.py` writes
    it: sorted keys, pretty-printed, with the collections array.
    """
    document = live_catalog()
    document["collections"] = [{"id": "physics", "name": "Physics", "description": "Fields."}]
    document["packs"][0]["collection"] = "physics"
    published = json.dumps(document, indent=2, sort_keys=True)
    assert published.index('"packs"') < published.index('"schema"')

    fields, _ = run(harness, published, 256, "header")
    assert fields["ok"] == "1"
    assert int(fields["version"]) == 7
    fields, _ = run(harness, published, 256, "validate")
    assert fields["ok"] == "1"
    first = document["packs"][0]
    assert find(harness, published, pack_id=first["id"])[0] == first["id"]
    assert find(harness, published, url=document["packs"][-1]["url"])[0] == document["packs"][-1]["id"]


def test_header_pass_ignores_entry_problems(harness):
    document = live_catalog()
    del document["packs"][3]["sha256"]
    fields, _ = run(harness, document, 256, "header")
    assert fields["ok"] == "1"
    assert int(fields["version"]) == 7


@pytest.mark.parametrize("mutate, reason", [
    (lambda d: d.update(schema=2), "wrong schema"),
    (lambda d: d.update(packs=[]), "no entries"),
    (lambda d: d.pop("packs"), "no packs array"),
    (lambda d: d["packs"][2].pop("sha256"), "entry without sha256"),
    (lambda d: d["packs"][2].pop("url"), "entry without url"),
    (lambda d: d["packs"][2].pop("id"), "entry without id"),
    (lambda d: d["packs"][2].pop("bytes"), "entry without bytes"),
    (lambda d: d["packs"][2].update(bytes="12345"), "bytes as a string"),
    (lambda d: d["packs"][2].update(url="https://example.invalid/" + "x" * 120), "url beyond its cap"),
])
def test_rejected_documents(harness, mutate, reason):
    document = live_catalog()
    mutate(document)
    fields, _ = run(harness, document, 256, "validate")
    assert fields["ok"] == "0", reason
    # A pack after the damage cannot be resolved either: the reader refuses the
    # document it cannot trust rather than answering from part of it.
    packs = document.get("packs") or []
    last_id = packs[-1].get("id", "no-such-pack") if packs else "no-such-pack"
    assert find(harness, document, pack_id=last_id) is None, reason


def test_a_pack_before_a_broken_entry_is_still_usable(harness):
    """Lookups stop at the entry they want; a later broken entry is not their problem."""
    document = live_catalog()
    document["packs"][-1].pop("sha256")
    assert find(harness, document, pack_id=document["packs"][0]["id"]) is not None
    assert find(harness, document, pack_id=document["packs"][-1]["id"]) is None


@pytest.mark.parametrize("text", [
    '{"schema": 1, "catalog_version": 1, "packs": [{"id": "a",',
    '{"schema": 1, "catalog_version": 1, "packs": [{"id": "a", "url": "u", "sha256": "s", "bytes": 1}',
    '{"schema": 1, "catalog_version": 1, "packs": [',
    "",
])
def test_truncated_documents_are_rejected(harness, text):
    fields, _ = run(harness, text, 256, "validate")
    assert fields["ok"] == "0"


def test_missing_catalog_version_still_reads_packs(harness):
    document = live_catalog()
    del document["catalog_version"]
    fields, _ = run(harness, document, 256, "header")
    assert fields["ok"] == "1"
    assert int(fields["version"]) == 0
    assert find(harness, document, pack_id=document["packs"][0]["id"]) is not None
