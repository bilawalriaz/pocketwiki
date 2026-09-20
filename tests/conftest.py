"""Shared pytest fixtures for PocketWiki host tests."""

import os
import shutil
import subprocess
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(REPO, "tools")
FIRMWARE_MAIN = os.path.join(REPO, "firmware", "main")
sys.path.insert(0, TOOLS)

import pack_content  # noqa: E402


@pytest.fixture(scope="session")
def sample_archive(tmp_path_factory):
    """Pack the two sample articles under tests/fixtures/ once per session."""
    out = tmp_path_factory.mktemp("sample")
    pack_content.build(os.path.join(REPO, "tests", "fixtures", "articles", "sample"), str(out))
    import archive_format as af

    with open(out / "content.bin", "rb") as fh:
        content = fh.read()
    with open(out / "index.bin", "rb") as fh:
        index = fh.read()
    return af.Archive(content, index)


def _write_articles(tmp_path, files):
    d = tmp_path / "articles"
    d.mkdir(exist_ok=True)
    for name, content in files.items():
        p = d / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding="utf-8")
    return str(d)


@pytest.fixture
def make_archive(tmp_path):
    """Build an archive from an in-memory dict of article files."""
    import archive_format as af

    case_number = 0

    def build(files):
        nonlocal case_number
        case_number += 1
        case_dir = tmp_path / f"case-{case_number}"
        case_dir.mkdir()
        src = _write_articles(case_dir, files)
        out = case_dir / "out"
        pack_content.build(src, str(out))
        with open(out / "content.bin", "rb") as fh:
            content = fh.read()
        with open(out / "index.bin", "rb") as fh:
            index = fh.read()
        return af.Archive(content, index)

    return build


@pytest.fixture(scope="session")
def c_harness(tmp_path_factory):
    """The firmware's title_lookup.c compiled for the host."""
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("no C compiler available")
    bindir = tmp_path_factory.mktemp("bin")
    exe = bindir / "c_tl_harness"
    subprocess.run(
        [cc, "-O2", "-I", FIRMWARE_MAIN,
         os.path.join(os.path.dirname(__file__), "c_tl_harness.c"),
         os.path.join(FIRMWARE_MAIN, "title_lookup.c"),
         "-o", str(exe)],
        check=True, capture_output=True)
    return str(exe)


@pytest.fixture(scope="session")
def catalog_harness(tmp_path_factory):
    """The firmware's catalogue reader compiled for the host.

    The published catalogue is read by this code on the device, so the tests
    that build a catalogue also read it back through the harness.
    """
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("no C compiler available")
    bindir = tmp_path_factory.mktemp("catalog-bin")
    exe = bindir / "catalog_harness"
    subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-I", FIRMWARE_MAIN,
         os.path.join(FIRMWARE_MAIN, "catalog_index.c"),
         os.path.join(os.path.dirname(__file__), "c_catalog_index_harness.c"),
         "-o", str(exe)],
        check=True, capture_output=True)
    return str(exe)
