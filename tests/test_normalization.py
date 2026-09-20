"""Shared normalization vectors, run against BOTH the Python packer and the
firmware's title_lookup.c compiled for the host.

The vectors pin the exact byte-level contract: trim ASCII whitespace, collapse
ASCII whitespace runs, fold ASCII case; everything else passes through.
"""

import subprocess

import archive_format as af

# (raw title, expected normalized bytes)
VECTORS = [
    ("Hello World", b"hello world"),
    ("  HELLO   WORLD  ", b"hello world"),
    ("\tTab\t\tHere\n", b"tab here"),
    ("A", b"a"),
    ("Z", b"z"),
    ("ABC def", b"abc def"),
    ("", b""),
    ("   ", b""),
    ("\t\n\r\v\f", b""),
    ("Esp32-S3", b"esp32-s3"),
    ("Mixed\tCase\nTitle", b"mixed case title"),
    ("Under_score", b"under_score"),
    ("a\u00A0b", "a\u00a0b".encode()),          # NBSP is NOT ASCII whitespace
    ("\u00c4\u00c5", "\u00c4\u00c5".encode()),  # no unicode case folding
    ("P\u00ef \u00af", "p\u00ef \u00af".encode()),
    ("x" * 255, b"x" * 255),
    ("  \t Leading And Trailing \v ", b"leading and trailing"),
]


def _hex(b: bytes) -> str:
    return b.hex()


def test_python_normalization_vectors():
    for raw, expected in VECTORS:
        assert af.normalize_title(raw) == expected, repr(raw)


def test_c_normalization_vectors(c_harness, tmp_path):
    idx = tmp_path / "empty.index"
    idx.write_bytes(af.pack_index([]))
    stdin = "".join(f"n {_hex(raw.encode('utf-8'))}\n" for raw, _ in VECTORS)
    out = subprocess.run([c_harness, str(idx)], input=stdin.encode(),
                         capture_output=True, check=True).stdout.decode().splitlines()
    assert len(out) == len(VECTORS)
    for (raw, expected), got in zip(VECTORS, out):
        assert got == _hex(expected), f"C normalize({raw!r}) = {got}"


def test_c_search_matches_python(sample_archive, c_harness):
    """Same lookups through C binary search and Python search agree."""
    import tempfile
    import os

    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as fh:
        fh.write(sample_archive.index)
        path = fh.name
    try:
        queries = ["esp", "ESP", "welcome", "welcome to pocketwiki", "g", "greek",
                   "zzz-no-such", "\u03c0", "  html  ", "clean", "the history"]
        lines = []
        for q in queries:
            qb = q.encode("utf-8")
            lines.append(f"e {qb.hex()}")
            lines.append(f"p {qb.hex()}")
        out = subprocess.run([c_harness, path], input="\n".join(lines).encode(),
                             capture_output=True, check=True).stdout.decode().splitlines()
        assert len(out) == 2 * len(queries)
        for q, exact, prefix in zip(queries, out[0::2], out[1::2]):
            py_exact = sample_archive.find_exact(q)
            py_exact_s = str(py_exact["id"]) if py_exact else "-"
            assert exact == py_exact_s, f"exact({q!r}): C={exact} py={py_exact_s}"
            py_prefix = [str(e["id"]) for e in sample_archive.find_prefix(q, 20)]
            py_prefix_s = " ".join(py_prefix) if py_prefix else "-"
            assert prefix == py_prefix_s, f"prefix({q!r}): C={prefix} py={py_prefix_s}"
    finally:
        os.unlink(path)


def test_normalization_idempotent():
    cases = [b"hello world", b"a b", b"\xc3\x84 test", b"  x  "]
    for c in cases:
        once = af.normalize_title(c)
        assert af.normalize_title(once) == once
