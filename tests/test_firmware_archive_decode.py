"""Firmware per-article decode paths, compiled for the host.

Links the exact libraries the firmware uses — libzstd from
managed_components/rderr__esp-idf-zstd and the vendored miniz — and drives the
same loops as serve_article() in web_server.c against real packs of both
generations (v2 zstd frames + dictionary, v1 gzip streams), so the on-device
decode contract is verified without hardware.
"""

import os
import shutil
import subprocess
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
from content_paths import ARTICLES, available
ZSTD_LIB = os.path.join(REPO, "firmware", "managed_components",
                        "rderr__esp-idf-zstd", "zstd", "lib")
MINIZ_DIR = os.path.join(REPO, "firmware", "third_party", "miniz")
HARNESS_C = os.path.join(os.path.dirname(__file__), "c_archive_decode_harness.c")

sys.path.insert(0, os.path.join(REPO, "tools"))
import pack_content  # noqa: E402


@pytest.fixture(scope="session")
def decode_harness(tmp_path_factory):
    if not os.path.isdir(ZSTD_LIB) or not os.path.isdir(MINIZ_DIR):
        pytest.skip("firmware third-party libraries not present (run a firmware build)")
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("no C compiler available")
    exe = tmp_path_factory.mktemp("bin") / "c_archive_decode_harness"
    srcs = [HARNESS_C, os.path.join(MINIZ_DIR, "miniz.c")]
    for d in ("common", "decompress"):
        for name in sorted(os.listdir(os.path.join(ZSTD_LIB, d))):
            if name.endswith(".c"):
                srcs.append(os.path.join(ZSTD_LIB, d, name))
    cmd = [cc, "-O2", "-DZSTD_DISABLE_ASM", "-I", ZSTD_LIB,
           "-I", os.path.join(ZSTD_LIB, "common"),
           "-I", os.path.join(ZSTD_LIB, "decompress"),
           "-I", MINIZ_DIR,
           *srcs, "-o", str(exe)]
    subprocess.run(cmd, check=True, capture_output=True)
    return str(exe)


def _build_pack(tmp_path, codec, articles_dir=None):
    out = tmp_path / f"pack-{codec}"
    pack_content.build(articles_dir or os.path.join(REPO, "tests", "fixtures", "articles", "sample"), str(out),
                       codec=codec)
    import archive_format as af
    content = (out / "content.bin").read_bytes()
    index = (out / "index.bin").read_bytes()
    return af.Archive(content, index), str(out / "content.bin"), str(out / "index.bin")


def test_v2_zstd_decode_matches_python(decode_harness, tmp_path):
    arch, content_path, index_path = _build_pack(tmp_path, "zstd")
    assert arch.format_version == 2
    for e in arch.entries:
        got = subprocess.run([decode_harness, content_path, index_path, str(e["id"])],
                             capture_output=True)
        assert got.returncode == 0, got.stderr.decode()
        assert got.stdout == arch.article_bytes(e)


@pytest.mark.skipif(
    not available(),
    reason="needs the pocketwiki-content article corpus (set POCKETWIKI_CONTENT_DIR)",
)
def test_v2_zstd_decoder_accepts_real_biology_pack(decode_harness, tmp_path):
    """Exercise the firmware's 1 KiB block guard on a representative corpus."""
    source = str(ARTICLES / "db-packs" / "biology-health")
    arch, content_path, index_path = _build_pack(tmp_path, "zstd", source)
    assert arch.count == 100
    for e in arch.entries:
        got = subprocess.run([decode_harness, content_path, index_path, str(e["id"])],
                             capture_output=True)
        assert got.returncode == 0, f"article {e['id']}: {got.stderr.decode()}"
        assert got.stdout == arch.article_bytes(e)


def test_v1_gzip_decode_matches_python(decode_harness, tmp_path):
    arch, content_path, index_path = _build_pack(tmp_path, "gzip")
    assert arch.format_version == 1
    for e in arch.entries:
        got = subprocess.run([decode_harness, content_path, index_path, str(e["id"])],
                             capture_output=True)
        assert got.returncode == 0, got.stderr.decode()
        assert got.stdout == arch.article_bytes(e)


def test_v2_rejects_truncated(decode_harness, tmp_path):
    arch, content_path, index_path = _build_pack(tmp_path, "zstd")
    index = bytearray(arch.index)
    e0 = arch.entries[0]
    off = 32 + e0["id"] * 40 + 8  # comp_len field
    old = int.from_bytes(index[off:off + 4], "little")
    index[off:off + 4] = (old // 2).to_bytes(4, "little")
    (tmp_path / "index.bin").write_bytes(bytes(index))
    index_path = str(tmp_path / "index.bin")
    got = subprocess.run([decode_harness, content_path, index_path, str(e0["id"])],
                         capture_output=True)
    assert got.returncode != 0
    assert b"truncated" in got.stderr or b"decode" in got.stderr


def test_v2_rejects_corrupt_frame(decode_harness, tmp_path):
    arch, content_path, index_path = _build_pack(tmp_path, "zstd")
    content = bytearray(arch.content)
    e0 = arch.entries[0]
    content[32 + e0["content_offset"] + e0["comp_len"] // 2] ^= 0xFF
    (tmp_path / "content.bin").write_bytes(bytes(content))
    content_path = str(tmp_path / "content.bin")
    got = subprocess.run([decode_harness, content_path, index_path, str(e0["id"])],
                         capture_output=True)
    assert got.returncode != 0


def test_v1_rejects_corrupt_frame(decode_harness, tmp_path):
    arch, content_path, index_path = _build_pack(tmp_path, "gzip")
    content = bytearray(arch.content)
    e0 = arch.entries[0]
    content[16 + e0["content_offset"] + e0["comp_len"] - 9] ^= 0xFF  # deflate body
    (tmp_path / "content.bin").write_bytes(bytes(content))
    content_path = str(tmp_path / "content.bin")
    got = subprocess.run([decode_harness, content_path, index_path, str(e0["id"])],
                         capture_output=True)
    assert got.returncode != 0
