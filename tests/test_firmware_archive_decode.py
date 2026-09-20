"""Firmware per-article decode path, compiled for the host.

tests/c_archive_decode_harness.c mirrors stream_article_deflate() in
firmware/main/web_server.c — the same inflate ring, dictionary pre-load,
1 KiB input chunks and end conditions — and links the same miniz source the
firmware compiles. Anything that would decode differently on a board fails
here.
"""

import os
import shutil
import subprocess
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MINIZ_DIR = os.path.join(REPO, "firmware", "third_party", "miniz")
HARNESS_C = os.path.join(os.path.dirname(__file__), "c_archive_decode_harness.c")

sys.path.insert(0, os.path.join(REPO, "tools"))
import pack_content  # noqa: E402
import archive_format as af  # noqa: E402
import content_paths  # noqa: E402

# A pack only trains a dictionary worth testing when the corpus is large enough,
# and that corpus lives in the pocketwiki-content checkout.
needs_corpus = pytest.mark.skipif(
    not content_paths.available(),
    reason=f"no pocketwiki-content checkout at {content_paths.CONTENT}",
)


@pytest.fixture(scope="session")
def decode_harness(tmp_path_factory):
    if not os.path.isdir(MINIZ_DIR):
        pytest.skip("vendored miniz is missing")
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("no C compiler available")
    exe = tmp_path_factory.mktemp("bin") / "c_archive_decode_harness"
    subprocess.run(
        [cc, "-O2", "-I", MINIZ_DIR, HARNESS_C,
         os.path.join(MINIZ_DIR, "miniz.c"), "-o", str(exe)],
        check=True, capture_output=True)
    return str(exe)


def _build_pack(tmp_path, articles_dir=None, **kwargs):
    out = tmp_path / "pack"
    pack_content.build(articles_dir or os.path.join(REPO, "tests", "fixtures", "articles", "sample"),
                       str(out), **kwargs)
    content = (out / "content.bin").read_bytes()
    index = (out / "index.bin").read_bytes()
    return af.Archive(content, index), str(out / "content.bin"), str(out / "index.bin")


def _decode(harness, content_path, index_path, eid):
    return subprocess.run([harness, content_path, index_path, str(eid)],
                          capture_output=True)


def test_deflate_decode_matches_python(decode_harness, tmp_path):
    arch, content_path, index_path = _build_pack(tmp_path)
    assert arch.format_version == af.FORMAT_VERSION
    for e in arch.entries:
        got = _decode(decode_harness, content_path, index_path, e["id"])
        assert got.returncode == 0, got.stderr.decode()
        assert got.stdout == arch.article_bytes(e)


@needs_corpus
def test_deflate_matches_python_on_biology_pack(decode_harness, tmp_path):
    """100 real articles against a ~23 KiB trained dictionary: the dictionary
    fills most of the ring, so any right-alignment mistake fails here."""
    source = str(content_paths.article_dir("db-packs/biology-health"))
    arch, content_path, index_path = _build_pack(tmp_path, source)
    assert arch.count == 100
    assert arch.dict_len >= 16384
    for e in arch.entries:
        got = _decode(decode_harness, content_path, index_path, e["id"])
        assert got.returncode == 0, f"article {e['id']}: {got.stderr.decode()}"
        assert got.stdout == arch.article_bytes(e)


def test_deflate_without_dictionary(decode_harness, tmp_path):
    """A dict-less pack (--dict-bytes 0) decodes with an untouched ring."""
    arch, content_path, index_path = _build_pack(tmp_path, dict_bytes_max=0)
    assert arch.dict_len == 0
    for e in arch.entries:
        got = _decode(decode_harness, content_path, index_path, e["id"])
        assert got.returncode == 0, got.stderr.decode()
        assert got.stdout == arch.article_bytes(e)


def test_rejects_corrupt_frame(decode_harness, tmp_path):
    arch, content_path, index_path = _build_pack(tmp_path)
    content = bytearray(arch.content)
    e0 = arch.entries[0]
    content[32 + e0["content_offset"] + e0["comp_len"] // 2] ^= 0xFF
    (tmp_path / "corrupt.bin").write_bytes(bytes(content))
    got = _decode(decode_harness, str(tmp_path / "corrupt.bin"), index_path, e0["id"])
    assert got.returncode != 0


def test_rejects_truncated_frame(decode_harness, tmp_path):
    """Half a frame must not decode as if the article were shorter."""
    arch, content_path, index_path = _build_pack(tmp_path)
    index = bytearray(arch.index)
    e0 = arch.entries[0]
    off = 32 + e0["id"] * 40 + 8  # comp_len field
    old = int.from_bytes(index[off:off + 4], "little")
    index[off:off + 4] = (old // 2).to_bytes(4, "little")
    (tmp_path / "short.bin").write_bytes(bytes(index))
    got = _decode(decode_harness, content_path, str(tmp_path / "short.bin"), e0["id"])
    assert got.returncode != 0


@needs_corpus
def test_rejects_wrong_dictionary(decode_harness, tmp_path):
    """A pack whose dictionary was swapped must fail the CRC check, not serve
    plausible-looking garbage. Needs a corpus that trains a real dictionary."""
    source = str(content_paths.article_dir("db-packs/biology-health"))
    arch, content_path, index_path = _build_pack(tmp_path, source)
    assert arch.dict_len >= 16384
    content = bytearray(arch.content)
    payload_end = arch.payload_offset + arch.payload_size
    for i in range(arch.dict_len):
        content[payload_end - arch.dict_len + i] ^= 0x5A
    (tmp_path / "swapped.bin").write_bytes(bytes(content))
    e0 = arch.entries[0]
    got = _decode(decode_harness, str(tmp_path / "swapped.bin"), index_path, e0["id"])
    assert got.returncode != 0
