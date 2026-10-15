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


# (normalized title, needle, word-start match?)
WORD_VECTORS = [
    (b"connecting to wi-fi", b"wi", True),
    (b"connecting to wi-fi", b"fi", True),
    (b"connecting to wi-fi", b"to wi", True),
    (b"connecting to wi-fi", b"conn", False),      # prefix hit, not a word hit
    (b"connecting to wi-fi", b"ecting", False),    # mid-word
    (b"world war ii", b"war", True),
    (b"world war ii", b"world", False),
    (b"newton's laws", b"s", False),               # apostrophe stays in the word
    (b"pi (number)", b"number", True),
    (b"esp32-s3", b"s3", True),
    (b"esp32-s3", b"32", False),
    (b"caf\xc3\xa9 society", b"society", True),
    (b"caf\xc3\xa9", b"\xa9", False),              # never inside a UTF-8 character
    (b"a", b"a", False),
    (b"ab", b"abc", False),
    (b"x war war", b"war", True),
]


def test_python_word_start_vectors():
    for norm, needle, expected in WORD_VECTORS:
        assert af.word_start_match(norm, needle) is expected, (norm, needle)


def _index_of(titles: list[str]) -> tuple[bytes, list[bytes]]:
    norms = sorted({af.normalize_title(t) for t in titles})
    entries = [{"id": i, "content_offset": 0, "comp_len": 0, "uncomp_len": 0, "crc32": 0,
                "norm": n, "disp": n.upper()} for i, n in enumerate(norms)]
    return af.pack_index(entries), norms


def test_c_word_start_matches_python(c_harness, tmp_path):
    """The batched C scan agrees with the Python mirror, including across
    entry batches, string-window refills and the end of the index."""
    words = ["war", "wi-fi", "world", "water", "wind", "pi", "s3", "the", "café"]
    titles = [norm.decode() for norm, _, _ in WORD_VECTORS]
    titles += [f"Topic {i:03d} {words[i % len(words)]} {'x' * (i % 37)}" for i in range(300)]
    index, norms = _index_of(titles)
    path = tmp_path / "words.index"
    path.write_bytes(index)
    queries = ["wi", "war", "WAR", "w", "fi", "s3", "the", "café", "x", "xxxxxxxxxx",
               "topic", "001", "zzz", "é", "  Wi  "]
    stdin = "".join(f"w {q.encode().hex()}\n" for q in queries)
    for extra in ([], ["nosize"]):          # index size known, and unknown
        out = subprocess.run([c_harness, str(path), *extra], input=stdin.encode(),
                             capture_output=True, check=True).stdout.decode().splitlines()
        assert len(out) == len(queries)
        for q, got in zip(queries, out):
            needle = af.normalize_title(q)
            want = [str(i) for i, n in enumerate(norms) if af.word_start_match(n, needle)][:20]
            assert got == (" ".join(want) if want else "-"), f"word_start({q!r}) {extra}"


def test_search_ranks_exact_then_prefix_then_word_starts(make_archive):
    titles = ["War", "Warsaw", "World War II", "Cold war", "Award", "Water"]
    arch = make_archive({f"{i}.md": f"# {t}\n\nBody.\n" for i, t in enumerate(titles)})
    got = [e["disp"].decode() for e in arch.search("war")]
    assert got == ["War", "Warsaw", "Cold war", "World War II"]
    assert [e["disp"].decode() for e in arch.search("war", 2)] == ["War", "Warsaw"]
