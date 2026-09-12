"""PWPK v1 experimental host pack writer and reader.

The format deliberately keeps the index boring: fixed width entries are easy to
validate and can be binary searched directly from flash.  ``build_pack`` is
also used by the benchmark runner, so it accepts already encoded article bytes
and never changes the source corpus.
"""
from __future__ import annotations

import dataclasses
import gzip
import hashlib
import lzma
import struct
import zlib
from bisect import bisect_left
from typing import Iterable

try:
    import zstandard
except ImportError:  # pragma: no cover - useful error is raised on use
    zstandard = None
try:
    import brotli
except ImportError:  # pragma: no cover
    brotli = None

MAGIC = b"PWPK"
VERSION = 1
HEADER_SIZE = 96
ENTRY_SIZE = 48
MAX_UNIT_BYTES = 16 * 1024 * 1024
MAX_DICTIONARY_BYTES = 65536
_HEADER = struct.Struct("<4sBBHIIQ" + "Q" * 8 + "II")
_ENTRY = struct.Struct("<QQIIIIHHII4s")
CODECS = {"zstd": 1, "brotli": 2, "xz": 3, "gzip": 4}
CODEC_NAMES = {v: k for k, v in CODECS.items()}


@dataclasses.dataclass(frozen=True)
class Article:
    id: int
    title: str | bytes
    text: bytes

    def __post_init__(self):
        if not 0 <= self.id <= 0xFFFFFFFFFFFFFFFF:
            raise ValueError("article id out of range")
        if isinstance(self.text, str):
            object.__setattr__(self, "text", self.text.encode("utf-8"))
        if not isinstance(self.text, bytes):
            raise TypeError("article text must be bytes")


@dataclasses.dataclass(frozen=True)
class Entry:
    id: int
    unit_offset: int
    compressed_size: int
    raw_offset: int
    raw_size: int
    title_offset: int
    title_size: int
    crc32: int
    unit_raw_length: int

    @property
    def compressed_length(self): return self.compressed_size
    @property
    def article_offset(self): return self.raw_offset
    @property
    def raw_length(self): return self.raw_size


@dataclasses.dataclass(frozen=True)
class PackStats:
    header_bytes: int
    index_bytes: int
    title_bytes: int
    dictionary_bytes: int
    payload_bytes: int
    unit_count: int
    total_bytes: int


def _compress(data: bytes, codec: str, level: int, dictionary: bytes) -> bytes:
    if codec == "zstd":
        if zstandard is None:
            raise RuntimeError("zstandard package is required for zstd")
        d = zstandard.ZstdCompressionDict(dictionary) if dictionary else None
        return zstandard.ZstdCompressor(level=level, dict_data=d).compress(data)
    if dictionary:
        raise ValueError("dictionaries are supported only by zstd")
    if codec == "brotli":
        if brotli is None: raise RuntimeError("brotli package is required")
        return brotli.compress(data, quality=max(0, min(11, level)))
    if codec == "xz":
        return lzma.compress(data, format=lzma.FORMAT_XZ,
                             filters=[{"id": lzma.FILTER_LZMA2, "preset": max(0, min(9, level)),
                                       "dict_size": max(4096, 1 << (len(data) - 1).bit_length())}])
    if codec == "gzip":
        return gzip.compress(data, compresslevel=max(0, min(9, level)), mtime=0)
    raise ValueError(f"unsupported codec {codec!r}")


def _decompress(data: bytes, codec: str, dictionary: bytes, max_output: int) -> bytes:
    if not 0 < max_output <= MAX_UNIT_BYTES:
        raise ValueError("decoded unit exceeds resource limit")
    if codec == "zstd":
        if zstandard is None: raise RuntimeError("zstandard package is required")
        params = zstandard.get_frame_parameters(data)
        if params.content_size != max_output or params.window_size > MAX_UNIT_BYTES:
            raise ValueError("Zstd frame size/window mismatch")
        d = zstandard.ZstdCompressionDict(dictionary) if dictionary else None
        return zstandard.ZstdDecompressor(dict_data=d, max_window_size=MAX_UNIT_BYTES // 1024).decompress(
            data, max_output_size=max_output, allow_extra_data=False)
    if dictionary: raise ValueError("non-zstd pack has a dictionary")
    if codec == "brotli":
        decoder = brotli.Decompressor()
        out = decoder.process(data, output_buffer_limit=max_output + 1)
        while not decoder.is_finished() and not decoder.can_accept_more_data() and len(out) <= max_output:
            more = decoder.process(b"", output_buffer_limit=max_output + 1 - len(out))
            if not more: break
            out += more
        if not decoder.is_finished() or len(out) != max_output:
            raise ValueError("Brotli decoded size or termination mismatch")
        return out
    if codec == "xz":
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=128 * 1024 * 1024)
        out = decoder.decompress(data, max_length=max_output + 1)
    elif codec == "gzip":
        decoder = zlib.decompressobj(31)
        out = decoder.decompress(data, max_output + 1)
    else: raise ValueError(f"unsupported codec {codec!r}")
    if not decoder.eof or decoder.unused_data or len(out) != max_output:
        raise ValueError("decoded size or frame termination mismatch")
    return out


def build_pack(records: Iterable[Article], codec: str = "zstd", level: int = 9,
               block_size: int = 0, dictionary: bytes = b"") -> bytes:
    """Build deterministic PWPK bytes.

    ``records`` controls physical order (useful for semantic ordering). The
    index is always sorted by numeric article ID. ``block_size=0`` gives one
    independent compressed unit per article; otherwise articles are grouped
    until adding the next one would exceed the target, while never splitting an
    article.
    """
    if codec not in CODECS: raise ValueError(f"unsupported codec {codec!r}")
    records = list(records)
    if not records: raise ValueError("empty packs are not supported")
    if not 0 <= block_size <= MAX_UNIT_BYTES: raise ValueError("block size out of range")
    if len(dictionary) > MAX_DICTIONARY_BYTES: raise ValueError("dictionary too large")
    for a in records:
        if not 0 < len(a.text) <= MAX_UNIT_BYTES: raise ValueError("article size out of range")
    if len({a.id for a in records}) != len(records): raise ValueError("duplicate article id")
    if any(not a.text for a in records): raise ValueError("empty article text is not packable")
    if dictionary and codec != "zstd": raise ValueError("dictionary requires zstd")
    title_blob = bytearray()
    title_pos: dict[bytes, tuple[int, int]] = {}
    def title_ref(a: Article):
        t = a.title.encode("utf-8") if isinstance(a.title, str) else a.title
        t.decode("utf-8")
        if len(t) > 65535: raise ValueError("title too long")
        if t not in title_pos:
            title_pos[t] = (len(title_blob), len(t)); title_blob.extend(t)
        return title_pos[t]
    units: list[tuple[bytes, list[tuple[Article, int]]]] = []
    current: list[tuple[Article, int]] = []; current_raw = 0
    for a in records:
        if block_size and current and current_raw + len(a.text) > block_size:
            raw = b"".join(x.text for x, _ in current)
            units.append((_compress(raw, codec, level, dictionary), current)); current=[]; current_raw=0
        current.append((a, current_raw)); current_raw += len(a.text)
        if not block_size:  # independent units
            raw = a.text; units.append((_compress(raw, codec, level, dictionary), current)); current=[]; current_raw=0
    if current:
        raw = b"".join(x.text for x, _ in current); units.append((_compress(raw, codec, level, dictionary), current))
    payload = bytearray(); entries = []
    for comp, members in units:
        off = len(payload); payload.extend(comp)
        unit_raw_length = sum(len(a.text) for a, _ in members)
        for a, raw_off in members:
            toff, tlen = title_ref(a)
            entries.append((a.id, off, len(comp), raw_off, len(a.text), toff, tlen,
                            zlib.crc32(a.text) & 0xffffffff, unit_raw_length))
    entries.sort(key=lambda x: x[0])
    index = b"".join(_ENTRY.pack(*e[:7], 0, e[7], e[8], b"\0" * 4) for e in entries)
    index_off = HEADER_SIZE; titles_off = index_off + len(index)
    dict_off = titles_off + len(title_blob) if dictionary else 0
    payload_off = titles_off + len(title_blob) + len(dictionary)
    body = index + bytes(title_blob) + dictionary + bytes(payload)
    body_crc = zlib.crc32(body) & 0xffffffff
    identity = (codec.encode() + struct.pack("<qI", level, block_size) + dictionary +
                b"".join(struct.pack("<Q", a.id) +
                         (a.title.encode("utf-8") if isinstance(a.title, str) else a.title) + a.text
                         for a in records))
    pack_id = int.from_bytes(hashlib.blake2b(
        identity,
        digest_size=8).digest(), "little") if records else 0
    header = _HEADER.pack(MAGIC, VERSION, CODECS[codec], 0, HEADER_SIZE,
                          len(entries), pack_id, index_off, len(index),
                          titles_off, len(title_blob), dict_off, len(dictionary),
                          payload_off, len(payload), body_crc, 0)
    return header + body


class Reader:
    """Validated random access reader over a complete PWPK byte string."""
    def __init__(self, data: bytes, validate_crc: bool = True):
        if len(data) < HEADER_SIZE: raise ValueError("truncated PWPK header")
        vals = _HEADER.unpack_from(data)
        (magic, ver, codec_id, flags, hs, count, pack_id, index_off, index_len,
         titles_off, titles_len, dict_off, dict_len, payload_off, payload_len,
         body_crc, _reserved) = vals
        if magic != MAGIC or ver != VERSION: raise ValueError("invalid PWPK magic/version")
        if hs != HEADER_SIZE or codec_id not in CODEC_NAMES or flags or _reserved or not count: raise ValueError("invalid PWPK header")
        if index_off != HEADER_SIZE or index_len != count * ENTRY_SIZE: raise ValueError("invalid index size")
        end = payload_off + payload_len
        if titles_off != index_off + index_len or dict_off not in (0, titles_off + titles_len):
            raise ValueError("invalid PWPK offsets")
        if payload_off != titles_off + titles_len + dict_len: raise ValueError("invalid PWPK layout")
        if end != len(data): raise ValueError("truncated or trailing PWPK data")
        if dict_len > MAX_DICTIONARY_BYTES or (dict_len and dict_off != titles_off + titles_len): raise ValueError("invalid dictionary range")
        if dict_len == 0 and dict_off != 0: raise ValueError("invalid dictionary offset")
        if dict_len and codec_id != CODECS["zstd"]: raise ValueError("dictionary requires zstd codec")
        if validate_crc and (zlib.crc32(data[index_off:]) & 0xffffffff) != body_crc:
            raise ValueError("PWPK CRC mismatch")
        self.data, self.codec, self.count = data, CODEC_NAMES[codec_id], count
        self.pack_id = pack_id
        self.payload_off, self.payload_len = payload_off, payload_len
        self._titles_size = titles_len
        self.dictionary = data[dict_off:dict_off + dict_len] if dict_len else b""
        self._entries: list[Entry] = []
        for i in range(count):
            p = index_off + i * ENTRY_SIZE
            eid, uo, cs, ro, rs, to, ts, reserved, crc, unit_raw, pad = _ENTRY.unpack_from(data, p)
            if reserved or pad != b"\0" * 4: raise ValueError("nonzero reserved index bytes")
            if i and eid <= self._entries[-1].id: raise ValueError("index IDs are not sorted")
            if uo + cs > payload_len or to + ts > titles_len or ro + rs > 0xFFFFFFFF:
                raise ValueError("PWPK entry out of bounds")
            if not cs or not rs or not 0 < unit_raw <= MAX_UNIT_BYTES or ro + rs > unit_raw: raise ValueError("article exceeds decoded unit")
            self._entries.append(Entry(eid, uo, cs, ro, rs, to, ts, crc, unit_raw))
        self._ids = [e.id for e in self._entries]
        self.entries = tuple(self._entries)
        self._titles_off = titles_off
        for e in self._entries:
            try: data[titles_off + e.title_offset:titles_off + e.title_offset + e.title_size].decode("utf-8")
            except UnicodeDecodeError as exc: raise ValueError("invalid UTF-8 title") from exc
        units = {}
        for e in self._entries:
            units.setdefault(e.unit_offset, []).append(e)
        next_offset = 0
        for unit_offset, group in sorted(units.items()):
            group.sort(key=lambda e: e.raw_offset)
            first = group[0]
            if unit_offset != next_offset:
                raise ValueError("compression units overlap or have gaps")
            next_offset += first.compressed_size
            if any(e.compressed_size != first.compressed_size or e.unit_raw_length != first.unit_raw_length for e in group):
                raise ValueError("inconsistent compression unit metadata")
            if (first.raw_offset != 0 or
                any(a.raw_offset + a.raw_size != b.raw_offset for a,b in zip(group,group[1:])) or
                group[-1].raw_offset + group[-1].raw_size != first.unit_raw_length):
                raise ValueError("unit articles are not a contiguous partition")
        if next_offset != payload_len:
            raise ValueError("unindexed payload bytes")

    def lookup(self, article_id: int) -> Entry:
        i = bisect_left(self._ids, article_id)
        if i == len(self._ids) or self._ids[i] != article_id: raise KeyError(article_id)
        return self._entries[i]

    def title(self, entry_or_id: Entry | int) -> str:
        e = self.lookup(entry_or_id) if isinstance(entry_or_id, int) else entry_or_id
        return self.data[self._titles_off + e.title_offset:self._titles_off + e.title_offset + e.title_size].decode("utf-8")

    def compressed_range(self, article_id: int) -> tuple[int, int]:
        e = self.lookup(article_id); return self.payload_off + e.unit_offset, e.compressed_size

    def extract(self, article_id: int) -> bytes:
        e = self.lookup(article_id)
        raw = _decompress(self.data[self.payload_off + e.unit_offset:self.payload_off + e.unit_offset + e.compressed_size], self.codec, self.dictionary, e.unit_raw_length)
        if len(raw) != e.unit_raw_length: raise ValueError("decoded unit length mismatch")
        out = raw[e.raw_offset:e.raw_offset + e.raw_size]
        if len(out) != e.raw_size or (zlib.crc32(out) & 0xffffffff) != e.crc32: raise ValueError("PWPK article CRC mismatch")
        return out

    def stats(self) -> PackStats:
        return PackStats(HEADER_SIZE, self.count * ENTRY_SIZE, self._titles_len(), len(self.dictionary), self.payload_len, len({(e.unit_offset, e.compressed_size) for e in self._entries}), len(self.data))

    @property
    def metrics(self):
        s = self.stats()
        return {"header_bytes": s.header_bytes, "index_bytes": s.index_bytes,
                "title_bytes": s.title_bytes, "dictionary_bytes": s.dictionary_bytes,
                "payload_bytes": s.payload_bytes, "unit_count": s.unit_count,
                "total_bytes": s.total_bytes}

    def _titles_len(self):
        return self._titles_size
