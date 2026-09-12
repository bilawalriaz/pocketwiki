"""Host-side tests for the packer and archive format."""

import json
import re
import zlib
import os
import subprocess
import sys

import pytest

import archive_format as af
import pack_content
import zstandard
from sanitize import md_to_html, sanitize_html

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GOOD = {
    "alpha.md": "# Alpha\n\nFirst article about **alpha** particles.\n",
    "beta.md": "# Beta\n\nSecond article.\n",
    "gamma.html": "<!doctype html><html><body><h1>Gamma</h1><p>Third.</p></body></html>",
}


def run_cli(*args):
    return subprocess.run([sys.executable, os.path.join(REPO, "tools", "pack_content.py"),
                           *args], capture_output=True, text=True)


# ---- determinism / structure ----------------------------------------------

def test_firmware_magic_constants_match_archive_format():
    header = open(os.path.join(REPO, "firmware", "main", "content_archive.h"),
                  encoding="utf-8").read()
    for symbol, magic in (("PW_CONTENT_MAGIC", af.MAGIC_CONTENT),
                          ("PW_INDEX_MAGIC", af.MAGIC_INDEX)):
        match = re.search(rf"#define\s+{symbol}\s+0x([0-9A-Fa-f]+)u", header)
        assert match, symbol
        assert int(match.group(1), 16) == int.from_bytes(magic, "little")


def test_firmware_reads_payload_after_version_specific_header():
    source = open(os.path.join(REPO, "firmware", "main", "content_archive.c"),
                  encoding="utf-8").read()
    function = source[source.index("esp_err_t ca_read_payload"):
                      source.index("int ca_index_read")]
    assert "PW_CONTENT_HEADER_SIZE_LEGACY" in function
    assert "format_version" in function


def test_firmware_uses_memory_constrained_zstd_decoder():
    cmake = open(os.path.join(REPO, "firmware", "CMakeLists.txt"),
                 encoding="utf-8").read()
    match = re.search(r"ZSTD_DECODER_INTERNAL_BUFFER=(\d+)", cmake)
    assert match
    assert int(match.group(1)) <= 4096

def test_deterministic_output(tmp_path):
    d1 = tmp_path / "o1"
    d2 = tmp_path / "o2"
    pack_content.build(os.path.join(REPO, "tests", "fixtures", "articles", "sample"), str(d1))
    pack_content.build(os.path.join(REPO, "tests", "fixtures", "articles", "sample"), str(d2))
    for name in ("content.bin", "index.bin", "manifest.json"):
        assert (d1 / name).read_bytes() == (d2 / name).read_bytes(), name


def test_manifest(tmp_path):
    out = tmp_path / "out"
    pack_content.build(os.path.join(REPO, "tests", "fixtures", "articles", "sample"), str(out))
    m = json.loads((out / "manifest.json").read_text())
    assert m["article_count"] == 2
    assert m["compressed_total"] > 0 and m["uncompressed_total"] > m["compressed_total"]
    assert m["compression_ratio"] > 1.0


def test_no_per_article_files(tmp_path):
    out = tmp_path / "out"
    pack_content.build(os.path.join(REPO, "tests", "fixtures", "articles", "sample"), str(out))
    names = {p.name for p in out.iterdir()}
    assert names == {"content.bin", "index.bin", "manifest.json"}


# ---- rejection cases -------------------------------------------------------

def test_duplicate_normalized_titles(make_archive):
    files = dict(GOOD)
    files["dup1.md"] = "# Alpha\n\nAnother alpha.\n"
    files["dup2.html"] = "<h1>  ALPHA </h1><p>Third alpha.</p>"
    with pytest.raises(af.ArchiveError, match="duplicate"):
        make_archive(files)


def test_empty_articles(make_archive):
    with pytest.raises(af.ArchiveError, match="empty"):
        make_archive({"x.md": "# X\n\n\n\n"})
    with pytest.raises(af.ArchiveError, match="empty"):
        make_archive({"x.html": "<html><head><script>alert(1)</script></head>"
                                "<body></body></html>"})


def test_long_title_rejected(make_archive):
    title = "A" * 300
    with pytest.raises(af.ArchiveError, match="255"):
        make_archive({"short.md": f"# {title}\n\nbody\n"})


def test_long_title_accepted(make_archive):
    title = "A" * 200
    arch = make_archive({f"t.md": f"# {title}\n\nbody\n"})
    assert arch.count == 1
    assert arch.entries[0]["norm"] == b"a" * 200


def test_invalid_utf8_rejected(make_archive, tmp_path):
    d = tmp_path / "articles"
    d.mkdir()
    (d / "bad.md").write_bytes(b"# Bad\n\n\xff\xfe broken\n")
    out = tmp_path / "out"
    with pytest.raises(af.ArchiveError, match="UTF-8"):
        pack_content.build(str(d), str(out))


# ---- unicode ---------------------------------------------------------------

def test_unicode_roundtrip(make_archive):
    arch = make_archive({
        "zh.md": "# 汉字测试\n\n这是中文内容，包含 **粗体**。\n",
        "gr.md": "# Ελληνικά\n\nΤο ελληνικό αλφάβητο έχει 24 γράμματα: α β γ δ.\n",
    })
    zh = arch.find_exact("汉字测试")
    assert zh is not None
    body = arch.article_bytes(zh).decode("utf-8")
    assert "这是中文内容" in body
    gr = arch.find_exact("Ελληνικά")
    assert gr is not None and "α β γ δ" in arch.article_bytes(gr).decode("utf-8")
    # no unicode case folding: lowercase ελληνικά is a distinct key
    assert arch.find_exact("ελληνικά") is None
    # prefix search works on unicode
    assert arch.find_prefix("汉字")[0]["id"] == zh["id"]


# ---- compression -----------------------------------------------------------

def test_compression_roundtrip(sample_archive):
    for e in sample_archive.entries:
        body = sample_archive.article_bytes(e)   # validates zstd decode + len + CRC
        assert len(body) == e["uncomp_len"]
        assert e["comp_len"] > 0 and e["uncomp_len"] > 0


def test_large_article_compresses(make_archive):
    arch = make_archive({"history.md": "# The History of Computing\n\n" +
                         ("A repeated historical paragraph. " * 2000)})
    big = arch.find_exact("the history of computing")
    assert big["uncomp_len"] > 20000
    assert big["comp_len"] < big["uncomp_len"] / 2


def test_zstd_frames_use_bounded_window(make_archive):
    # A large article must not produce a frame whose declared decode window
    # scales with article size: the ESP32 decoder allocates that window on the
    # heap while streaming, so an unbounded window OOMs the device ("Out of
    # memory" on the dashboard). The packer caps ZSTD_WINDOW_LOG so every
    # frame's window stays at 16 KiB regardless of article length.
    big = "<h1>Big</h1>\n" + "<p>%s</p>\n" % ("repeat phrase " * 4000) + "x" * 1_000_000
    arch = make_archive({"big.html": big})
    cap = 1 << pack_content.ZSTD_WINDOW_LOG
    max_window = 0
    for e in arch.entries:
        raw = arch.content[arch.payload_offset + e["content_offset"]:
                        arch.payload_offset + e["content_offset"] + e["comp_len"]]
        max_window = max(max_window,
                         zstandard.get_frame_parameters(raw).window_size)
    assert max_window <= cap, f"frame window {max_window} exceeds cap {cap}"


def test_small_corpus_uses_small_dictionary(sample_archive):
    # The C3 pins the dict blob + digested DDict + stream window in heap, so
    # a small corpus must not carry a full 32 KiB dictionary it cannot
    # amortize: the 100-article starter OOMed every /a/<id> ("Out of memory")
    # with a 32 KiB dict and serves with a ~7 KiB one.
    assert sample_archive.dict_len <= 8192
    assert pack_content.train_dict([b"x" * 100] * 8, max_bytes=0) == b""


# ---- index invariants ------------------------------------------------------

def test_index_ordering_and_ids(sample_archive):
    norms = [e["norm"] for e in sample_archive.entries]
    assert norms == sorted(norms)
    assert len(set(norms)) == len(norms)
    for i, e in enumerate(sample_archive.entries):
        assert e["id"] == i


def test_lookup_semantics(sample_archive):
    # case-insensitive exact
    assert sample_archive.find_exact("arduino")["id"] == sample_archive.find_exact("Arduino")["id"]
    assert sample_archive.find_exact("  BATTERIES  ")["id"] == sample_archive.find_exact("batteries")["id"]
    # prefix
    got = sample_archive.find_prefix("ard")
    assert got and all(e["norm"].startswith(b"ard") for e in got)
    # limit
    assert len(sample_archive.find_prefix("t", 20)) <= 20
    # no match
    assert sample_archive.find_exact("zzz") is None
    assert sample_archive.find_prefix("zzz") == []
    # exact-first ordering in search()
    res = sample_archive.search("arduino")
    assert res[0]["id"] == sample_archive.find_exact("arduino")["id"]


# ---- corruption handling ---------------------------------------------------

def test_invalid_magic_content():
    content = bytearray(af.pack_content_header(0))
    content[:4] = b"XXXX"
    with pytest.raises(af.ArchiveError, match="magic"):
        af.Archive(bytes(content), af.pack_index([]))


def test_invalid_magic_index():
    index = bytearray(af.pack_index([]))
    index[:4] = b"YYYY"
    with pytest.raises(af.ArchiveError, match="magic"):
        af.Archive(af.pack_content_header(0), bytes(index))


def test_truncated_content():
    full = af.pack_content_header(10) + b"\x00" * 10
    for cut in (0, 5, 15, 25):
        with pytest.raises(af.ArchiveError):
            af.Archive(full[:cut], af.pack_index([]))


def test_truncated_index():
    index = af.pack_index([])
    for cut in (0, 10, 31):
        with pytest.raises(af.ArchiveError):
            af.Archive(af.pack_content_header(0), index[:cut])


def test_out_of_range_content_offset():
    entries = [dict(id=0, content_offset=1_000_000, comp_len=10, uncomp_len=5,
                    crc32=0, norm=b"a", disp=b"A")]
    index = af.pack_index(entries)
    content = af.pack_content_header(100) + b"\x00" * 100
    with pytest.raises(af.ArchiveError, match="exceeds payload"):
        af.Archive(content, index)


def test_out_of_range_title_offset():
    # hand-built index with a VALID whole-file CRC but a title offset far
    # outside the file — exercises the per-entry bounds check, not the CRC
    import struct
    entry = af._ENTRY.pack(0, 0, 5, 5, 0, 0xFFFFFFF0, 1, 32, 1, b"\0" * 8)
    strings = b"aA"
    body = entry + strings
    index = af._INDEX_HEADER.pack(af.MAGIC_INDEX, af.FORMAT_VERSION, af.ENTRY_SIZE,
                                  0, 1, 32, 32 + 40, 32 + len(body),
                                  zlib.crc32(body) & 0xFFFFFFFF, 0) + body
    content = af.pack_content_header(5) + b"\x00" * 5
    with pytest.raises(af.ArchiveError, match="out of file"):
        af.Archive(content, index)


def test_zero_length_rejected():
    entries = [dict(id=0, content_offset=0, comp_len=0, uncomp_len=5,
                    crc32=0, norm=b"a", disp=b"A")]
    with pytest.raises(af.ArchiveError, match="zero length"):
        af.Archive(af.pack_content_header(5) + b"\x00" * 5, af.pack_index(entries))


def test_crc_mismatch_index(sample_archive):
    index = bytearray(sample_archive.index)
    index[32] ^= 0x01  # corrupt an entry byte
    with pytest.raises(af.ArchiveError, match="CRC"):
        af.Archive(sample_archive.content, bytes(index))


def test_crc_mismatch_content(sample_archive):
    content = bytearray(sample_archive.content)
    # flip the last byte of the first article's zstd frame: the archive
    # structure stays valid, but decompression must fail the check
    e0 = sample_archive.entries[0]
    content[sample_archive.payload_offset + e0["comp_len"] - 1] ^= 0x01
    arch = af.Archive(bytes(content), sample_archive.index)   # structural check passes
    with pytest.raises(af.ArchiveError):
        arch.article_bytes(e0)


# ---- sanitizer / markdown --------------------------------------------------

def test_link_rewriting(make_archive):
    arch = make_archive({
        "home.md": "# Home\n\nSee [Beta](beta.md), [Gamma](/gamma.html), "
                   "[external](https://example.com), [dead](nowhere.md) and "
                   "[mail](mailto:x@y.z).\n",
        "beta.md": "# Beta\n\nBody.\n",
        "gamma.html": "<h1>Gamma</h1><p>Body.</p>",
    })
    home = arch.find_exact("home")
    body = arch.article_bytes(home).decode("utf-8")
    beta = arch.find_exact("beta")["id"]
    gamma = arch.find_exact("gamma")["id"]
    assert f'href="/a/{beta}"' in body
    assert f'href="/a/{gamma}"' in body
    assert "https://example.com" not in body
    assert "nowhere.md" not in body
    assert "mailto:" not in body
    # dropped links keep their text but lose the article anchor entirely
    assert body.count('href="/a/') == 2
    assert "external" in body and "dead" in body and "mail" in body


def test_articles_have_pocketwiki_page_chrome(sample_archive):
    e = sample_archive.entries[0]
    body = sample_archive.article_bytes(e).decode("utf-8")
    assert body.startswith("<!doctype html>")
    assert '<link rel="stylesheet" href="/style.css">' in body
    assert '<a class="brand" href="/" aria-label="PocketWiki home">' in body
    assert '<form class="search" action="/search" method="get">' in body
    assert '<nav aria-label="Main navigation"><a href="/manage">Manage library</a></nav>' in body
    assert '<nav class="article-nav"><a href="/">&larr; Back to your libraries</a></nav>' in body
    assert "<article>" in body and "</article>" in body
    assert f'href="/download/{e["id"]}"' in body
    assert "Download this article as HTML" in body
    assert "<footer>PocketWiki offline &middot; Reading stays local</footer>" in body


def test_pre_resolved_article_links_survive_sanitizing():
    out = sanitize_html('<p><a href="/a/42#History">History</a></p>')
    assert 'href="/a/42#History"' in out


def test_numeric_corpus_subset_remaps_pre_resolved_links(make_archive):
    arch = make_archive({
        "0002.html": '<h1>Alpha</h1><p><a href="/a/9">Beta</a> '
                     '<a href="/a/5">omitted</a></p>',
        "0009.html": '<h1>Beta</h1><p><a href="/a/2#History">Alpha</a></p>',
    })
    alpha = arch.article_bytes(arch.find_exact("alpha")).decode("utf-8")
    beta = arch.article_bytes(arch.find_exact("beta")).decode("utf-8")
    assert '<a href="/a/1">Beta</a>' in alpha
    assert '<a>omitted</a>' in alpha
    assert '<a href="/a/0#History">Alpha</a>' in beta


def test_sanitizer_strips_hostile_content():
    doc = """<html><head><script>evil()</script><style>x{}</style></head><body>
    <h1>Safe</h1>
    <p>Text <script>alert(1)</script> more <img src="http://x/p.png" alt="pic">
    <a href="https://ext.example" onclick="h()">out</a>
    <a href="local.md">in</a>
    <iframe src="http://y"></iframe> done</p>
    <table><tr><td>c</td></tr></table>
    <blockquote>q</blockquote>
    <hr>
    <!-- comment -->
    </body></html>"""
    out = sanitize_html(doc, resolver=lambda key: 7 if key == "local" else None)
    assert "evil" not in out and "<script" not in out and "<style" not in out
    assert "alert" not in out
    assert "pic" in out                      # img alt kept
    assert "https://ext.example" not in out and "onclick" not in out
    assert 'href="/a/7"' in out
    assert "<table><tr><td>c</td></tr></table>" in out
    assert "<blockquote>q</blockquote>" in out
    assert "<hr>" in out and "comment" not in out


def test_sanitizer_keeps_text_escaping():
    out = sanitize_html("<h1>T</h1><p>a < b &amp; c &gt; d</p>")
    assert "&lt;" in out and "&amp;" in out and "&gt;" in out
    assert "<p>" in out and "</p>" in out


def test_markdown_subset():
    md = """# Title

Para with **bold**, *italic*, `code` and [a link](target.md).

- one
- two
1. first
2. second

> quote line

```c
int x = 1;
```

---

*not bold* and <b>raw html stays text</b>
"""
    out = md_to_html(md, resolver=lambda key: 3 if key == "target" else None)
    assert "<h1>Title</h1>" in out
    assert "<strong>bold</strong>" in out and "<em>italic</em>" in out
    assert "<code>code</code>" in out
    assert 'href="/a/3"' in out
    assert "<ul>" in out and "<li>one</li>" in out and "<li>two</li>" in out
    assert "<ol>" in out and "<li>first</li>" in out
    assert "<blockquote>quote line</blockquote>" in out
    assert "<pre><code>int x = 1;</code></pre>" in out
    assert "<hr>" in out
    assert "&lt;b&gt;raw html stays text&lt;/b&gt;" in out


def test_markdown_pipe_table():
    out = md_to_html("""# Table

| Property | Value |
| :--- | ---: |
| **A** | `one` |
| B | two \\| three |
""")
    assert "<table><thead><tr><th>Property</th><th>Value</th></tr></thead>" in out
    assert "<tbody>" in out and "<th>" in out
    assert "<strong>A</strong>" in out and "<code>one</code>" in out
    assert "two | three" in out


def test_first_h1_kept_in_body(sample_archive):
    e = sample_archive.find_exact("arduino")
    body = sample_archive.article_bytes(e).decode("utf-8")
    assert "<h1>Arduino</h1>" in body


def test_clean_html_article_is_clean(make_archive):
    arch = make_archive({"clean.html": "<html><body><h1>Clean HTML Demo</h1>"
                         "<script>hacked(); steal();</script><p>safe text "
                         "<a href='https://evil.example/pixel'>evil link</a></p>"
                         "<p>tracking pixel</p></body></html>"})
    e = arch.search("clean html demo")[0]
    body = arch.article_bytes(e).decode("utf-8")
    # Hostile article content is stripped: the <script> payload, remote
    # refs, and javascript: links are gone from the article body.
    assert "hacked" not in body
    assert "steal()" not in body and "javascript:" not in body
    assert "evil.example" not in body
    # The only script on the page is the page shell's own download handler.
    assert body.count("<script") == 1
    assert 'id="pw-download"' in body and "URL.createObjectURL" in body
    assert "tracking pixel" in body           # permitted article text kept


# ---- CLI -------------------------------------------------------------------

def test_cli_build_verify_inspect(tmp_path):
    out = tmp_path / "cli-out"
    r = run_cli("build", os.path.join(REPO, "tests", "fixtures", "articles", "sample"), str(out))
    assert r.returncode == 0, r.stderr
    r = run_cli("verify", str(out))
    assert r.returncode == 0 and "OK:" in r.stdout
    r = run_cli("inspect", str(out))
    assert r.returncode == 0 and "articles" in r.stdout
    assert os.path.exists(out / "content.bin") and os.path.exists(out / "index.bin")


def test_cli_duplicate_fails(tmp_path):
    src = tmp_path / "a"
    src.mkdir()
    (src / "one.md").write_text("# Same\n\nx\n")
    (src / "two.md").write_text("# SAME\n\nx\n")
    r = run_cli("build", str(src), str(tmp_path / "o"))
    assert r.returncode == 1 and "duplicate" in r.stderr


def test_cli_partition_image_fits(tmp_path):
    out = tmp_path / "o"
    r = run_cli("build", os.path.join(REPO, "tests", "fixtures", "articles", "sample"), str(out))
    assert r.returncode == 0
    r = run_cli("partition-image", str(out),
                "--partitions", os.path.join(REPO, "firmware", "partitions.csv"))
    assert r.returncode == 0, r.stderr
    assert os.path.getsize(out / "content.part") == 0x70000
    assert os.path.getsize(out / "index.part") == 0x10000


def test_cli_partition_image_overflow(tmp_path):
    out = tmp_path / "o"
    pack_content.build(os.path.join(REPO, "tests", "fixtures", "articles", "sample"), str(out))
    (out / "content.bin").write_bytes(b"\x00" * (0x70000 + 1))
    r = run_cli("partition-image", str(out),
                "--partitions", os.path.join(REPO, "firmware", "partitions.csv"))
    assert r.returncode == 1 and "exceeds" in r.stderr


# ---- portable pack (single-file Wi-Fi upload) -----------------------------

def test_pack_roundtrip(sample_archive):
    data = af.pack_pack_file(sample_archive.content, sample_archive.index)
    assert data[:4] == af.MAGIC_PACK
    content, index = af.unpack_pack_file(data)
    assert content == sample_archive.content
    assert index == sample_archive.index
    arch = af.Archive(content, index)
    assert arch.count == sample_archive.count


def test_pack_truncated():
    data = af.pack_pack_file(af.pack_content_header(10) + b"\x00" * 10, af.pack_index([]))
    for cut in (0, 10, 31):
        with pytest.raises(af.ArchiveError):
            af.unpack_pack_file(data[:cut])


def test_pack_size_mismatch():
    data = bytearray(af.pack_pack_file(af.pack_content_header(10) + b"\x00" * 10,
                                       af.pack_index([])))
    data += b"\x00"  # trailing garbage: sizes no longer match file length
    with pytest.raises(af.ArchiveError, match="sizes"):
        af.unpack_pack_file(bytes(data))


def test_pack_crc_corruption():
    data = bytearray(af.pack_pack_file(af.pack_content_header(10) + b"\x00" * 10,
                                       af.pack_index([])))
    data[-1] ^= 0x01
    with pytest.raises(af.ArchiveError, match="CRC"):
        af.unpack_pack_file(bytes(data))


def test_pack_embedded_archive_validates():
    # a structurally valid pack whose embedded content.bin is corrupt must be
    # rejected when opened with Archive()
    content = bytearray(af.pack_content_header(10) + b"\x00" * 10)
    content[:4] = b"XXXX"
    data = af.pack_pack_file(bytes(content), af.pack_index([]))
    c, i = af.unpack_pack_file(data)
    with pytest.raises(af.ArchiveError, match="magic"):
        af.Archive(c, i)


def test_cli_pack_file(tmp_path):
    out = tmp_path / "o"
    run_cli("build", os.path.join(REPO, "tests", "fixtures", "articles", "sample"), str(out))
    pack = tmp_path / "science.bin"
    r = run_cli("pack-file", str(out), str(pack))
    assert r.returncode == 0, r.stderr
    assert pack.read_bytes()[:4] == af.MAGIC_PACK
    content, index = af.unpack_pack_file(pack.read_bytes())
    arch = af.Archive(content, index)
    assert arch.count > 0


def test_firmware_pack_magic_matches():
    header = open(os.path.join(REPO, "firmware", "main", "content_archive.h"),
                  encoding="utf-8").read()
    match = re.search(r"#define\s+PW_PACK_MAGIC\s+0x([0-9A-Fa-f]+)u", header)
    assert match
    assert int(match.group(1), 16) == int.from_bytes(af.MAGIC_PACK, "little")
