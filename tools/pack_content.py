#!/usr/bin/env python3
"""PocketWiki content packer.

Builds content.bin + index.bin from a directory of HTML/Markdown articles.

    python tools/pack_content.py build   articles/ build/content/
    python tools/pack_content.py verify  build/content/
    python tools/pack_content.py inspect build/content/
    python tools/pack_content.py partition-image build/content/ --partitions firmware/partitions.csv

Output is deterministic: articles sorted by normalized title, ids assigned in
that order, one shared zstd dictionary trained on the article bodies, no
timestamps anywhere. Requires the `zstandard` Python package.
"""

from __future__ import annotations

import argparse
import gzip
import html as html_lib
import json
import os
import re
import sys
import zlib
from html.parser import HTMLParser

import zstandard

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import archive_format as af       # noqa: E402
from sanitize import md_to_html, sanitize_html, escape_html  # noqa: E402

ZSTD_LEVEL = 19
# Max zstd window for emitted frames. MUST stay small: the ESP32 decoder
# streams each frame through ZSTD_decompressStream, which allocates a window
# buffer of this size on the heap. A bare ZstdCompressor(level=19) emits a
# single-segment frame whose declared window equals the FULL uncompressed
# article size, so a 200 KiB article needs a ~200 KiB buffer and the device
# OOMs ("Out of memory" on the dashboard). 14 (16 KiB) bounds the decoder's
# peak buffer to ~48 KiB regardless of article length; ratio loss vs an
# unbounded window is negligible because the shared trained dictionary
# captures cross-article redundancy.
ZSTD_WINDOW_LOG = 14
# Firmware sets ZSTD_d_maxBlockSize to this value. The Python binding does not
# expose ZSTD_c_maxBlockSize, so the builder uses explicit streaming block
# flushes instead of one-shot compression. This keeps emitted frames accepted
# by the same decoder configuration used on the device.
ZSTD_BLOCK_SIZE = 1024
# Trained dictionary size cap. The firmware keeps this raw blob in RAM and
# builds a by-reference ZSTD_DDict over it plus a streaming window, so every
# dict byte is pinned C3 heap. A 32 KiB cap leaves room for the digested
# entropy tables and streaming window on the C3; 64 KiB dictionaries failed
# there at boot. Small corpora use a proportionally smaller dictionary (see
# train_dict): it saves both flash and the pinned RAM that OOMs the C3.
ZSTD_DICT_MAX = 32768
ZSTD_DICT_MIN = 4096
ZSTD_DICT_TRAIN_SAMPLES = 1500   # cap for training time on large corpora


def train_dict(bodies: list[bytes], max_bytes: int = ZSTD_DICT_MAX) -> bytes:
    """Train a zstd dictionary on the exact bytes the packer will compress.

    Uses zstandard's cover trainer (deterministic for identical inputs) on the
    first `ZSTD_DICT_TRAIN_SAMPLES` bodies. Returns b"" — a dict-less pack —
    when the corpus is too small to train usefully; the format supports both.

    The dictionary is sized proportionally to the corpus (total // 32,
    clamped to [ZSTD_DICT_MIN, max_bytes]): a 224 KiB starter gets ~7 KiB
    instead of a full 32 KiB blob it cannot amortize. The 100-article starter
    packs to 33 KiB total with a 4 KiB dict versus 48 KiB with a 32 KiB dict,
    and pins 28 KiB less C3 heap (raw blob + digested tables), which is the
    difference between serving articles and "Out of memory" on every /a/<id>.
    Pass max_bytes=0 to force a dict-less pack, or an explicit byte size to
    override the adaptive choice.
    """
    samples = bodies[:ZSTD_DICT_TRAIN_SAMPLES]
    total = sum(len(s) for s in samples)
    if len(samples) < 8 or total < ZSTD_DICT_MAX // 2:
        return b""
    if max_bytes == 0:
        return b""
    target = min(max_bytes, max(ZSTD_DICT_MIN, total // 32))
    try:
        return zstandard.train_dictionary(target, samples).as_bytes()
    except zstandard.ZstdError:
        return b""


class _TitleExtractor(HTMLParser):
    """First <h1> text; fallback <title> text."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.h1: list[str] = []
        self.title: list[str] = []
        self._in_h1 = 0
        self._in_title = 0

    def handle_starttag(self, tag, attrs):
        if self._in_h1 or self._in_title:
            return  # collect text through nested markup
        tag = tag.lower()
        if tag == "h1":
            self._in_h1 = 1
        elif tag == "title":
            self._in_title = 1

    def handle_endtag(self, tag):
        tag = tag.lower()
        if tag == "h1":
            self._in_h1 = 0
        elif tag == "title":
            self._in_title = 0

    def handle_data(self, data):
        if self._in_h1:
            self.h1.append(data)
        elif self._in_title:
            self.title.append(data)

    def result(self) -> str | None:
        for chunks in (self.h1, self.title):
            text = re.sub(r"\s+", " ", "".join(chunks)).strip()
            if text:
                return text
        return None


_MD_H1 = re.compile(r"^#{1}\s+(.+?)\s*#*\s*$", re.MULTILINE)


def extract_title(rel: str, text: str) -> tuple[str, str | None]:
    """Return (display_title, extracted_title_or_None)."""
    low = rel.lower()
    if low.endswith((".html", ".htm")):
        p = _TitleExtractor()
        p.feed(text)
        return p.result() or _stem(rel), p.result()
    if low.endswith(".md"):
        m = _MD_H1.search(text)
        if m:
            return re.sub(r"\s+", " ", m.group(1)).strip(), m.group(1).strip()
        return _stem(rel), None
    raise ValueError(f"unsupported article file: {rel} (use .html/.htm/.md)")


def _stem(rel: str) -> str:
    return os.path.splitext(os.path.basename(rel))[0].replace("_", " ").replace("-", " ")


def collect_files(articles_dir: str) -> list[str]:
    files: list[str] = []
    for root, _dirs, names in os.walk(articles_dir):
        for name in sorted(names):
            if name.lower().endswith((".html", ".htm", ".md")):
                files.append(os.path.join(root, name))
    files.sort()
    return files


def load_article(articles_dir: str, path: str) -> dict:
    rel = os.path.relpath(path, articles_dir)
    with open(path, "rb") as fh:
        raw = fh.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise af.ArchiveError(f"{rel}: not valid UTF-8 ({exc})")
    disp, extracted = extract_title(rel, text)
    norm = af.normalize_title(disp)
    if not norm:
        raise af.ArchiveError(f"{rel}: empty title after normalization")
    if len(norm) > af.MAX_TITLE_BYTES:
        raise af.ArchiveError(
            f"{rel}: normalized title {len(norm)} bytes > {af.MAX_TITLE_BYTES} ({disp[:40]!r})")
    if len(disp.encode("utf-8")) > af.MAX_DISPLAY_TITLE_BYTES:
        raise af.ArchiveError(f"{rel}: display title too long")
    return dict(rel=rel, path=path, disp=disp, norm=norm)


_ARCHIVE_HREF = re.compile(
    r'href=(["\'])/a/(\d+)(#[A-Za-z0-9_.:%-]+)?\1', re.IGNORECASE)


def _remap_archive_hrefs(text: str, old_to_new: dict[int, int] | None) -> str:
    """Remap importer-resolved ids when a numeric-file corpus is subsetted."""
    if old_to_new is None:
        return text

    def replace(match):
        new_id = old_to_new.get(int(match.group(2)))
        if new_id is None:
            return f'href={match.group(1)}#{match.group(1)}'
        fragment = match.group(3) or ""
        return f'href={match.group(1)}/a/{new_id}{fragment}{match.group(1)}'

    return _ARCHIVE_HREF.sub(replace, text)


def convert_and_rewrite(article: dict, id_map: dict,
                        old_to_new: dict[int, int] | None = None) -> str:
    """Produce a complete, sanitized PocketWiki HTML page for one article.

    Articles are stored as independent zstd frames sharing a trained
    dictionary; the ESP32 streams them through its zstd decoder and serves
    identity HTML.  The page chrome therefore belongs inside the compressed
    article rather than being prepended by the HTTP server.
    """
    rel = article["rel"]
    low = rel.lower()
    base_dir = os.path.dirname(rel).replace(os.sep, "/")

    def resolver(key: str):
        return id_map.get(key) or id_map.get(af.normalize_title(key))

    with open(article["path"], "rb") as fh:
        raw = fh.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    text = raw.decode("utf-8")
    text = _remap_archive_hrefs(text, old_to_new)

    if low.endswith((".html", ".htm")):
        body = sanitize_html(text, resolver=resolver, base_dir=base_dir)
    else:
        body = md_to_html(text, resolver=resolver)
    # A leading h1 is the indexed title, not article content. Reject files
    # that contain only that title (or only markup with no readable text).
    substantive = re.sub(r"^\s*<h1(?:\s[^>]*)?>.*?</h1>", "", body,
                          count=1, flags=re.IGNORECASE | re.DOTALL)
    substantive = html_lib.unescape(re.sub(r"<[^>]+>", "", substantive)).strip()
    if not substantive:
        raise af.ArchiveError(f"{rel}: empty article body")
    title = escape_html(article["disp"])
    body = (
        '<!doctype html><html><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        '<link rel="stylesheet" href="/style.css">'
        f'<title>PocketWiki &middot; {title}</title></head><body>'
        '<header><div class="header-inner">'
        '<a class="brand" href="/" aria-label="PocketWiki home">'
        '<span class="brand-mark" aria-hidden="true">P</span>'
        '<span>PocketWiki</span></a>'
        '<nav aria-label="Main navigation"><a href="/manage">Manage library</a></nav>'
        '<form class="search" action="/search" method="get">'
        '<label class="sr-only" for="site-search">Search article titles</label>'
        '<input id="site-search" type="search" name="q" '
        'placeholder="Search the library" autocomplete="off" maxlength="128">'
        '<button type="submit">Search</button></form></div></header>'
        '<main><nav class="article-nav"><a href="/">&larr; Back to your libraries</a></nav>'
        f'<article>{body}</article>'
        f'<p class="download-row"><a class="download" id="pw-download" data-id="{article["id"]}" href="/download/{article["id"]}">'
        'Download this article as HTML</a></p></main>'
        '<script>'
        '(function(){'
        'var link=document.getElementById("pw-download");'
        'if(!link){return}'
        'link.addEventListener("click",function(ev){'
        'ev.preventDefault();'
        'var href=link.getAttribute("href");'
        'fetch(href).then(function(r){if(!r.ok){throw new Error("Download failed")}return r.blob()})'
        '.then(function(blob){'
        'var url=URL.createObjectURL(blob);'
        'var a=document.createElement("a");'
        'a.href=url;a.download="pocketwiki-"+link.getAttribute("data-id")+".html";'
        'document.body.appendChild(a);a.click();a.remove();'
        'setTimeout(function(){URL.revokeObjectURL(url)},10000)'
        '}).catch(function(){window.location.href=href})'
        '})'
        '})()'
        '</script>'
        '<footer>PocketWiki offline &middot; Reading stays local</footer></body></html>'
    )
    if len(body.encode("utf-8")) > af.MAX_ARTICLE_BYTES:
        raise af.ArchiveError(f"{rel}: article exceeds {af.MAX_ARTICLE_BYTES} bytes")
    return body


def build(articles_dir: str, out_dir: str, codec: str = "zstd",
          dict_bytes_max: int = ZSTD_DICT_MAX,
          zstd_level: int = ZSTD_LEVEL,
          zstd_window_log: int = ZSTD_WINDOW_LOG,
          zstd_block_size: int = ZSTD_BLOCK_SIZE) -> dict:
    files = collect_files(articles_dir)
    if not files:
        raise af.ArchiveError(f"no articles found under {articles_dir}")

    # pass 1: titles + duplicate detection
    articles = [load_article(articles_dir, f) for f in files]
    seen: dict[bytes, str] = {}
    for a in articles:
        prev = seen.get(a["norm"])
        if prev is not None:
            raise af.ArchiveError(
                f"duplicate normalized title {a['norm']!r}: {prev} and {a['rel']}")
        seen[a["norm"]] = a["rel"]

    # sort deterministically by normalized title
    articles.sort(key=lambda a: a["norm"])
    for i, a in enumerate(articles):
        a["id"] = i

    # Wikipedia imports use their stable source-list id as a numeric filename.
    # A capacity-selected subset gets new dense ids, so links resolved during
    # import must be translated (or removed when their target was omitted).
    numeric_ids: list[int] = []
    for a in articles:
        stem = os.path.splitext(os.path.basename(a["rel"]))[0]
        if not stem.isdigit():
            numeric_ids = []
            break
        numeric_ids.append(int(stem))
    old_to_new = ({old_id: a["id"] for old_id, a in zip(numeric_ids, articles)}
                  if numeric_ids and len(set(numeric_ids)) == len(numeric_ids) else None)

    # id lookup for link rewriting: path keys + normalized titles
    id_map: dict[str, int] = {}
    for a in articles:
        key = os.path.splitext(a["rel"])[0].replace(os.sep, "/")
        id_map[key] = a["id"]
        id_map[af.normalize_title(a["disp"])] = a["id"]  # type: ignore[dict-item]

    # pass 2: convert, compress each article as an independent frame. The
    # per-article frame model is identical for both codecs — the index maps an
    # article to one compressed blob read straight from flash, and the
    # firmware streams it through the matching decoder.
    #
    # codec "zstd" (default): one shared trained dictionary + zstd-19 frames
    # (format v2). codec "gzip": legacy v1 output, byte-compatible with the
    # original packer (gzip -9, mtime=0, 16-byte header) — kept so a pack can
    # be regenerated for old firmware or as a rollback path.
    bodies: list[bytes] = []
    for a in articles:
        body = convert_and_rewrite(a, id_map, old_to_new)
        bodies.append(body.encode("utf-8"))

    if codec == "gzip":
        version = af.FORMAT_VERSION_LEGACY
        dict_bytes = b""
    elif codec == "zstd":
        version = af.FORMAT_VERSION
        dict_bytes = train_dict(bodies, max_bytes=dict_bytes_max)
    else:
        raise af.ArchiveError(f"unknown codec {codec!r} (use 'zstd' or 'gzip')")
    if codec == "zstd":
        # Fixed 16 KiB window (see ZSTD_WINDOW_LOG): forces an explicit window
        # descriptor instead of a single-segment frame, so on-device decode
        # memory is bounded by the window, not by article size.
        zstd_c = zstandard.ZstdCompressor(
            compression_params=zstandard.ZstdCompressionParameters.from_level(
                zstd_level, window_log=zstd_window_log),
            dict_data=zstandard.ZstdCompressionDict(dict_bytes) if dict_bytes else None)

        def compress_zstd(data: bytes) -> bytes:
            if zstd_block_size <= 0:
                return zstd_c.compress(data)
            obj = zstd_c.compressobj()
            encoded = bytearray()
            for offset in range(0, len(data), zstd_block_size):
                encoded += obj.compress(data[offset:offset + zstd_block_size])
                encoded += obj.flush(zstandard.COMPRESSOBJ_FLUSH_BLOCK)
            encoded += obj.flush(zstandard.COMPRESSOBJ_FLUSH_FINISH)
            return bytes(encoded)

    entries = []
    payload = bytearray()
    for a, data in zip(articles, bodies):
        if codec == "zstd":
            comp = compress_zstd(data)
        else:
            comp = gzip.compress(data, compresslevel=9, mtime=0)
        e = dict(id=a["id"], content_offset=len(payload), comp_len=len(comp),
                 uncomp_len=len(data), crc32=zlib.crc32(data) & 0xFFFFFFFF,
                 norm=a["norm"], disp=a["disp"].encode("utf-8"))
        entries.append(e)
        payload += comp

    # Dictionary (v2 only) lives at the payload tail: per-article offsets
    # above are unaffected, and the decoder reads it once at archive load.
    dict_offset = len(payload) if dict_bytes else 0
    payload += dict_bytes
    content = af.pack_content_header(len(payload), dict_offset, len(dict_bytes),
                                     version) + bytes(payload)
    index = af.pack_index(entries, version)

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "content.bin"), "wb") as fh:
        fh.write(content)
    with open(os.path.join(out_dir, "index.bin"), "wb") as fh:
        fh.write(index)

    # full self-validation (bounds, ordering, CRC, decompression)
    verify_dir(out_dir)

    uncomp_total = sum(e["uncomp_len"] for e in entries)
    manifest = dict(
        format_version=version,
        codec="gzip-9" if codec == "gzip" else f"zstd-{zstd_level}",
        zstd_level=zstd_level if codec == "zstd" else None,
        zstd_window_log=zstd_window_log if codec == "zstd" else None,
        zstd_block_size=zstd_block_size if codec == "zstd" else None,
        dict_bytes=len(dict_bytes),
        article_count=len(entries),
        content_file=len(content),
        index_file=len(index),
        payload_size=len(payload),
        index_bytes_per_article=round(len(index) / len(entries), 1),
        uncompressed_total=uncomp_total,
        compressed_total=len(payload),
        compression_ratio=round(uncomp_total / len(payload), 2) if payload else 0,
    )
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True)
        fh.write("\n")
    return manifest


def verify_dir(out_dir: str) -> dict:
    with open(os.path.join(out_dir, "content.bin"), "rb") as fh:
        content = fh.read()
    with open(os.path.join(out_dir, "index.bin"), "rb") as fh:
        index = fh.read()
    arch = af.Archive(content, index)
    for e in arch.entries:
        arch.article_bytes(e)  # full zstd decode + length + CRC check
    return dict(article_count=arch.count, content_bytes=len(content), index_bytes=len(index))


def inspect_dir(out_dir: str) -> dict:
    with open(os.path.join(out_dir, "content.bin"), "rb") as fh:
        content = fh.read()
    with open(os.path.join(out_dir, "index.bin"), "rb") as fh:
        index = fh.read()
    arch = af.Archive(content, index)
    return arch


def make_partition_images(out_dir: str, partitions_csv: str):
    parts = af.parse_partitions_csv(partitions_csv)
    for name, archive_file in (("content", "content.bin"), ("index", "index.bin")):
        if name not in parts:
            raise af.ArchiveError(f"partitions.csv has no '{name}' partition")
        part = parts[name]
        with open(os.path.join(out_dir, archive_file), "rb") as fh:
            data = fh.read()
        if len(data) > part["size"]:
            raise af.ArchiveError(
                f"{archive_file} ({len(data)} bytes) exceeds {name} partition size "
                f"({part['size']} bytes) by {len(data) - part['size']} bytes — "
                f"shrink the content or enlarge the partition")
        img = af.make_partition_image(data, part["size"])
        out_path = os.path.join(out_dir, f"{name}.part")
        with open(out_path, "wb") as fh:
            fh.write(img)
        print(f"{name}.part: {len(data)} bytes archive -> {len(img)} partition image "
              f"({(len(img) - len(data))} bytes padding)")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="PocketWiki content packer")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("build", help="pack articles/ -> content.bin + index.bin")
    p.add_argument("articles_dir")
    p.add_argument("out_dir")
    p.add_argument("--codec", choices=("zstd", "gzip"), default="zstd",
                   help="zstd (v2, default) or legacy gzip (v1) article frames")
    p.add_argument("--dict-bytes", type=int, default=ZSTD_DICT_MAX,
                   help="max zstd dictionary bytes (0=dict-less; default adapts "
                        "down for small corpora to save C3 heap)")
    p.add_argument("--zstd-level", type=int, default=ZSTD_LEVEL,
                   help=f"zstd compression level (default: {ZSTD_LEVEL})")
    p.add_argument("--zstd-window-log", type=int, default=ZSTD_WINDOW_LOG,
                   help=f"zstd window log (default: {ZSTD_WINDOW_LOG})")
    p.add_argument("--zstd-block-size", type=int, default=ZSTD_BLOCK_SIZE,
                   help=f"zstd streaming block size; 0 uses one-shot compression "
                        f"(default: {ZSTD_BLOCK_SIZE})")
    p.add_argument("--partitions", help="firmware/partitions.csv (optional, enables partition images)")

    p = sub.add_parser("verify", help="fully validate an archive directory")
    p.add_argument("out_dir")

    p = sub.add_parser("inspect", help="print archive summary")
    p.add_argument("out_dir")

    p = sub.add_parser("partition-image", help="wrap archives into raw partition images")
    p.add_argument("out_dir")
    p.add_argument("--partitions", default=None, help="firmware/partitions.csv (required)")

    p = sub.add_parser("pack-file", help="wrap content.bin+index.bin into one portable pack")
    p.add_argument("out_dir", help="directory containing content.bin and index.bin")
    p.add_argument("out_file", help="path of the .bin pack to write")

    args = ap.parse_args(argv)

    try:
        if args.cmd == "build":
            manifest = build(args.articles_dir, args.out_dir, codec=args.codec,
                             dict_bytes_max=args.dict_bytes,
                             zstd_level=args.zstd_level,
                             zstd_window_log=args.zstd_window_log,
                             zstd_block_size=args.zstd_block_size)
            print(json.dumps(manifest, indent=2, sort_keys=True))
            if args.partitions:
                make_partition_images(args.out_dir, args.partitions)
        elif args.cmd == "verify":
            res = verify_dir(args.out_dir)
            print(f"OK: {res['article_count']} articles, "
                  f"content {res['content_bytes']} B, index {res['index_bytes']} B")
        elif args.cmd == "inspect":
            arch = inspect_dir(args.out_dir)
            print(f"content.bin : {len(arch.content)} bytes "
                  f"(header {af.CONTENT_HEADER_SIZE}, payload {arch.payload_size})")
            print(f"index.bin   : {len(arch.index)} bytes "
                  f"(entries {arch.count}, strings at {arch.strings_off})")
            print(f"articles    : {arch.count}")
            for e in arch.entries[:10]:
                print(f"  #{e['id']:4d} {e['disp'].decode('utf-8', 'replace')[:48]:50s} "
                      f"off={e['content_offset']:8d} comp={e['comp_len']:7d} uncomp={e['uncomp_len']:7d}")
            if arch.count > 10:
                print(f"  ... {arch.count - 10} more")
        elif args.cmd == "partition-image":
            if not args.partitions:
                ap.error("partition-image requires --partitions firmware/partitions.csv")
            make_partition_images(args.out_dir, args.partitions)
        elif args.cmd == "pack-file":
            with open(os.path.join(args.out_dir, "content.bin"), "rb") as fh:
                content = fh.read()
            with open(os.path.join(args.out_dir, "index.bin"), "rb") as fh:
                index = fh.read()
            # validate the source archives first
            af.Archive(content, index)
            pack = af.pack_pack_file(content, index)
            with open(args.out_file, "wb") as fh:
                fh.write(pack)
            print(f"pack: {len(content)} B content + {len(index)} B index "
                  f"-> {len(pack)} B {args.out_file}")
            back_content, back_index = af.unpack_pack_file(pack)
            af.Archive(back_content, back_index)
        return 0
    except (af.ArchiveError, ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
