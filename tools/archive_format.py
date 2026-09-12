"""PocketWiki archive formats v1/v2 — single source of truth for content.bin / index.bin.

See docs/ARCHIVE_FORMAT.md. All integers little-endian. The entry array is
fixed-width so the firmware can binary-search it in flash. v2 article
payloads are per-article zstd frames sharing one trained dictionary stored at
the end of the content payload; v1 (legacy) payloads are per-article gzip
streams with a 16-byte header. The firmware reads both; the packer emits both
(--codec zstd|gzip).
"""

from __future__ import annotations

import struct
import zlib

import zstandard

# --- format constants -------------------------------------------------------

FORMAT_VERSION = 2
FORMAT_VERSION_LEGACY = 1    # v1: 16-byte content header, gzip frames, no dict

MAGIC_CONTENT = b"PWKC"
MAGIC_INDEX = b"PWKI"
MAGIC_PACK = b"PWKP"          # single-file portable pack for Wi-Fi upload

CONTENT_HEADER_SIZE = 32
CONTENT_HEADER_SIZE_LEGACY = 16
INDEX_HEADER_SIZE = 32
ENTRY_SIZE = 40
PACK_HEADER_SIZE = 32

MAX_TITLE_BYTES = 255        # normalized title, UTF-8 bytes
MAX_DISPLAY_TITLE_BYTES = 511
MAX_ARTICLE_BYTES = 2 * 1024 * 1024  # uncompressed body cap

# v2: content header grew to 32 bytes to carry the zstd dictionary fields.
# Articles are independent zstd frames (level 19) compressed with a shared
# trained dictionary; the dictionary itself is stored at the end of the
# payload so per-article offsets are untouched and the decoder can stream.
# v1 (legacy): 16-byte header, per-article gzip streams, no dictionary. The
# packer can still emit v1 packs (--codec gzip) and the firmware still reads
# them, so installed packs keep working across a firmware upgrade and a
# rollback never requires repacking content.
_CONTENT_HEADER_LEGACY = struct.Struct("<4sB3sII")     # 16 bytes
_CONTENT_HEADER = struct.Struct("<4sB3sIIII8s")        # 32 bytes
_INDEX_HEADER = struct.Struct("<4sBBHIIIIII")          # 32 bytes
_ENTRY = struct.Struct("<IIIIIIHIH8s")                 # 40 bytes
_PACK_HEADER = struct.Struct("<4sB3sIII12s")           # 32 bytes

# ASCII whitespace — identical set to the C implementation (title_lookup.c)
WS_BYTES = b" \t\n\r\x0b\x0c"

# --- normalization (shared with firmware: title_lookup.c) -------------------

def normalize_title(raw: bytes | str) -> bytes:
    """Normalize a title to its canonical search form.

    Byte-exact contract (mirrored in firmware title_lookup.c):
      1. trim ASCII whitespace on both ends
      2. collapse runs of ASCII whitespace to a single 0x20
      3. ASCII case-fold A-Z -> a-z
    Non-ASCII bytes pass through unchanged. No Unicode case folding.
    """
    if isinstance(raw, str):
        raw = raw.encode("utf-8")
    s = raw.strip(WS_BYTES)
    out = bytearray()
    pending_space = False
    for b in s:
        if b in WS_BYTES:
            pending_space = True
            continue
        if pending_space and out:
            out.append(0x20)
        pending_space = False
        out.append(b + 0x20 if 0x41 <= b <= 0x5A else b)
    return bytes(out)


def same_normalized(a: bytes | str, b: bytes | str) -> bool:
    return normalize_title(a) == normalize_title(b)

# --- pack -------------------------------------------------------------------

def pack_content_header(payload_size: int, dict_offset: int = 0,
                        dict_len: int = 0,
                        version: int = FORMAT_VERSION) -> bytes:
    """content.bin header. The trained zstd dictionary (if any) is stored at
    the END of the payload, so article offsets are unaffected by its presence:
    dict_offset == payload_size - dict_len (offsets are payload-relative).

    version=FORMAT_VERSION_LEGACY emits the v1 16-byte header (gzip frames,
    no dictionary); version=FORMAT_VERSION the v2 32-byte header."""
    if version == FORMAT_VERSION_LEGACY:
        if dict_len:
            raise ArchiveError("legacy v1 packs cannot carry a dictionary")
        return _CONTENT_HEADER_LEGACY.pack(MAGIC_CONTENT, FORMAT_VERSION_LEGACY,
                                           b"\0\0\0", CONTENT_HEADER_SIZE_LEGACY,
                                           payload_size)
    if version != FORMAT_VERSION:
        raise ArchiveError(f"unsupported format version {version}")
    if dict_len:
        if dict_offset != payload_size - dict_len:
            raise ArchiveError("dict must be stored at the end of the payload")
    else:
        dict_offset = 0
    return _CONTENT_HEADER.pack(MAGIC_CONTENT, FORMAT_VERSION, b"\0\0\0",
                                CONTENT_HEADER_SIZE, payload_size,
                                dict_offset, dict_len, b"\0" * 8)


def pack_index(entries: list[dict],
               version: int = FORMAT_VERSION) -> bytes:
    """entries: sorted list of dicts with keys:
       id, content_offset, comp_len, uncomp_len, crc32,
       norm (bytes), disp (bytes)
    Returns the complete index.bin (header + entries + string table).
    """
    n = len(entries)
    assert n <= 0xFFFFFFFF
    strings = bytearray()
    string_off: dict[bytes, int] = {}

    def add_string(s: bytes) -> tuple[int, int]:
        # offsets are absolute file offsets (dedupe identical strings)
        rel = string_off.get(s)
        if rel is None:
            rel = len(strings)
            strings.extend(s)
            string_off[s] = rel
        return strings_start + rel, len(s)

    entry_bytes = bytearray()
    strings_start = INDEX_HEADER_SIZE + n * ENTRY_SIZE
    strings_off = strings_start
    for e in entries:
        norm_off, norm_len = add_string(e["norm"])
        disp_off, disp_len = add_string(e["disp"])
        entry_bytes += _ENTRY.pack(e["id"], e["content_offset"], e["comp_len"],
                                   e["uncomp_len"], e["crc32"],
                                   norm_off, norm_len, disp_off, disp_len, b"\0" * 8)

    body = bytes(entry_bytes) + bytes(strings)
    index_size = INDEX_HEADER_SIZE + len(body)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    header = _INDEX_HEADER.pack(MAGIC_INDEX, version, ENTRY_SIZE, 0,
                                n, INDEX_HEADER_SIZE, strings_off, index_size, crc, 0)
    return header + body

# --- single-file portable pack ---------------------------------------------

def pack_pack_file(content: bytes, index: bytes) -> bytes:
    """Wrap content.bin + index.bin into a single portable pack file.

    Layout (see docs/ARCHIVE_FORMAT.md §Portable pack):
        offset  size  field
        0       4     magic "PWKP"
        4       1     format version = embedded content.bin's version (1|2)
        5       3     reserved, zero
        8       4     content_len   # bytes of embedded content.bin
        12      4     index_len     # bytes of embedded index.bin
        16      4     crc32         # CRC32 of content+index payload
        20      12    reserved, zero
        32      ...   content.bin bytes
        ...     ...   index.bin bytes
    """
    if len(content) < 5:
        raise ArchiveError("pack: content.bin too small to carry a version")
    version = content[4]
    if version not in (FORMAT_VERSION, FORMAT_VERSION_LEGACY):
        raise ArchiveError(f"pack: unsupported content version {version}")
    payload = content + index
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = _PACK_HEADER.pack(MAGIC_PACK, version, b"\0\0\0",
                               len(content), len(index), crc, b"\0" * 12)
    return header + payload


def unpack_pack_file(data: bytes) -> tuple[bytes, bytes]:
    """Validate a portable pack and return (content.bin, index.bin) bytes."""
    if len(data) < PACK_HEADER_SIZE:
        raise ArchiveError("pack: truncated header")
    magic, ver, _res, content_len, index_len, crc, _r = _PACK_HEADER.unpack_from(data, 0)
    if magic != MAGIC_PACK:
        raise ArchiveError(f"pack: bad magic {magic!r}")
    if ver not in (FORMAT_VERSION, FORMAT_VERSION_LEGACY):
        raise ArchiveError(f"pack: unsupported version {ver}")
    min_content = CONTENT_HEADER_SIZE if ver == FORMAT_VERSION else CONTENT_HEADER_SIZE_LEGACY
    if content_len < min_content or index_len < INDEX_HEADER_SIZE:
        raise ArchiveError("pack: embedded sizes too small")
    if PACK_HEADER_SIZE + content_len + index_len != len(data):
        raise ArchiveError(
            f"pack: sizes {content_len}+{index_len}+{PACK_HEADER_SIZE} "
            f"!= file length {len(data)}")
    payload = data[PACK_HEADER_SIZE:]
    if (zlib.crc32(payload) & 0xFFFFFFFF) != crc:
        raise ArchiveError("pack: CRC mismatch")
    return payload[:content_len], payload[content_len:content_len + index_len]

# --- parse / validate -------------------------------------------------------

class ArchiveError(Exception):
    pass


def _u32(x: int, what: str) -> int:
    if not 0 <= x <= 0xFFFFFFFF:
        raise ArchiveError(f"{what}: out of u32 range: {x}")
    return x


class Archive:
    """Parsed, validated view of content.bin + index.bin."""

    def __init__(self, content: bytes, index: bytes):
        self.content = content
        self.index = index
        if len(content) < CONTENT_HEADER_SIZE_LEGACY:
            raise ArchiveError("content.bin truncated: missing header")
        if content[0:4] != MAGIC_CONTENT:
            raise ArchiveError(f"content.bin bad magic {content[0:4]!r}")
        ver = content[4]
        if ver not in (FORMAT_VERSION, FORMAT_VERSION_LEGACY):
            raise ArchiveError(f"content.bin unsupported version {ver}")
        if ver == FORMAT_VERSION:
            if len(content) < CONTENT_HEADER_SIZE:
                raise ArchiveError("content.bin truncated: missing header")
            (magic, ver, _res, hsize, payload_size, dict_offset, dict_len, _r) = \
                _CONTENT_HEADER.unpack_from(content, 0)
            header_size = CONTENT_HEADER_SIZE
            if hsize != CONTENT_HEADER_SIZE:
                raise ArchiveError(f"content.bin bad header_size {hsize}")
        else:
            (magic, _ver, _res, hsize, payload_size) = \
                _CONTENT_HEADER_LEGACY.unpack_from(content, 0)
            header_size = CONTENT_HEADER_SIZE_LEGACY
            dict_offset = dict_len = 0
            if hsize != CONTENT_HEADER_SIZE_LEGACY:
                raise ArchiveError(f"content.bin bad header_size {hsize}")
        if payload_size > len(content) - header_size:
            raise ArchiveError(
                f"content.bin payload_size {payload_size} exceeds file ({len(content) - header_size})")
        self.format_version = ver
        self.payload_offset = header_size
        self.payload_size = _u32(payload_size, "content payload_size")
        dict_offset = _u32(dict_offset, "content dict_offset")
        dict_len = _u32(dict_len, "content dict_len")
        if dict_len > self.payload_size:
            raise ArchiveError(f"content.bin dict_len {dict_len} exceeds payload {self.payload_size}")
        if dict_len:
            if dict_offset + dict_len != self.payload_size:
                raise ArchiveError("content.bin dict must be at the end of the payload")
        elif dict_offset:
            raise ArchiveError("content.bin dict_offset nonzero but dict_len is 0")
        self.dict_offset = dict_offset
        self.dict_len = dict_len
        self.dict = (content[self.payload_offset + dict_offset:
                             self.payload_offset + dict_offset + dict_len]
                     if dict_len else b"")
        self._zddict = (zstandard.ZstdCompressionDict(self.dict)
                        if dict_len else None)

        if len(index) < INDEX_HEADER_SIZE:
            raise ArchiveError("index.bin truncated: missing header")
        (magic, ver, entry_size, _r2, count, entries_off, strings_off,
         index_size, crc, _r3) = _INDEX_HEADER.unpack_from(index, 0)
        if magic != MAGIC_INDEX:
            raise ArchiveError(f"index.bin bad magic {magic!r}")
        if ver != self.format_version:
            raise ArchiveError(
                f"index.bin version {ver} does not match content version {self.format_version}")
        if entry_size != ENTRY_SIZE:
            raise ArchiveError(f"index.bin bad entry_size {entry_size}")
        if index_size != len(index):
            raise ArchiveError(f"index.bin index_size {index_size} != file length {len(index)}")
        if entries_off != INDEX_HEADER_SIZE:
            raise ArchiveError(f"index.bin entries_offset {entries_off} != {INDEX_HEADER_SIZE}")
        expected = INDEX_HEADER_SIZE + count * ENTRY_SIZE
        if expected > index_size:
            raise ArchiveError(f"index.bin entries overflow file: count={count} need {expected} > {index_size}")
        if not (0 <= strings_off <= index_size):
            raise ArchiveError(f"index.bin strings_offset {strings_off} out of range")
        body = index[INDEX_HEADER_SIZE:index_size]
        if (zlib.crc32(body) & 0xFFFFFFFF) != crc:
            raise ArchiveError("index.bin CRC mismatch")
        self.count = count
        self.entries_off = entries_off
        self.strings_off = strings_off

        # Parse and bounds-check every entry eagerly (host side; the firmware
        # checks the same bounds per request).
        self.entries: list[dict] = []
        prev_norm: bytes | None = None
        for i in range(count):
            ent = _ENTRY.unpack_from(index, entries_off + i * ENTRY_SIZE)
            (eid, c_off, c_len, u_len, crc32, n_off, n_len, d_off, d_len, _r) = ent
            if eid != i:
                raise ArchiveError(f"index entry {i}: id {eid} not sequential")
            if c_len == 0 or u_len == 0:
                raise ArchiveError(f"index entry {i}: zero length")
            if c_off + c_len > self.payload_size:
                raise ArchiveError(f"index entry {i}: content range {c_off}+{c_len} exceeds payload {self.payload_size}")
            if n_len < 1 or d_len < 1:
                raise ArchiveError(f"index entry {i}: empty title")
            if n_off + n_len > index_size or d_off + d_len > index_size:
                raise ArchiveError(f"index entry {i}: title range out of file")
            norm = index[n_off:n_off + n_len]
            disp = index[d_off:d_off + d_len]
            if normalize_title(norm) != norm:
                raise ArchiveError(f"index entry {i}: stored normalized title not normalized: {norm!r}")
            if prev_norm is not None and norm <= prev_norm:
                raise ArchiveError(f"index entry {i}: title order violated ({norm!r} <= {prev_norm!r})")
            prev_norm = norm
            self.entries.append(dict(id=eid, content_offset=c_off, comp_len=c_len,
                                     uncomp_len=u_len, crc32=crc32,
                                     norm=norm, disp=disp))

    # --- article access -----------------------------------------------------

    def article_bytes(self, e: dict) -> bytes:
        """Decompressed article body (host-side use only).

        v2: independent zstd frames compressed against the shared trained
        dictionary (self.dict). v1 (legacy): per-article gzip streams.
        Mirrors the firmware decode dispatch.
        """
        start = self.payload_offset + e["content_offset"]
        raw = self.content[start:start + e["comp_len"]]
        if len(raw) != e["comp_len"]:
            raise ArchiveError(f"article {e['id']}: truncated compressed data")
        if self.format_version == FORMAT_VERSION_LEGACY:
            try:
                out = zlib.decompress(raw, 16 + zlib.MAX_WBITS)  # gzip wrapper
            except zlib.error as exc:
                raise ArchiveError(f"article {e['id']}: gzip decode failed: {exc}") from exc
        else:
            try:
                dctx = zstandard.ZstdDecompressor(dict_data=self._zddict)
                out = dctx.decompress(raw, max_output_size=MAX_ARTICLE_BYTES)
            except (zstandard.ZstdError, ValueError) as exc:
                raise ArchiveError(f"article {e['id']}: zstd decode failed: {exc}") from exc
        if len(out) != e["uncomp_len"]:
            raise ArchiveError(f"article {e['id']}: length mismatch {len(out)} != {e['uncomp_len']}")
        if (zlib.crc32(out) & 0xFFFFFFFF) != e["crc32"]:
            raise ArchiveError(f"article {e['id']}: CRC mismatch")
        return out

    # --- search (mirrors firmware title_lookup.c semantics) -----------------

    def find_exact(self, title: bytes | str) -> dict | None:
        needle = normalize_title(title)
        lo, hi = 0, len(self.entries)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.entries[mid]["norm"] < needle:
                lo = mid + 1
            else:
                hi = mid
        if lo < len(self.entries) and self.entries[lo]["norm"] == needle:
            return self.entries[lo]
        return None

    def find_prefix(self, title: bytes | str, limit: int = 20) -> list[dict]:
        needle = normalize_title(title)
        lo, hi = 0, len(self.entries)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.entries[mid]["norm"] < needle:
                lo = mid + 1
            else:
                hi = mid
        out = []
        while lo < len(self.entries) and len(out) < limit:
            norm = self.entries[lo]["norm"]
            if not norm.startswith(needle):
                break
            out.append(self.entries[lo])
            lo += 1
        return out

    def search(self, title: bytes | str, limit: int = 20) -> list[dict]:
        """Exact match first, then prefix matches, deduplicated, capped at limit."""
        needle = normalize_title(title)
        if not needle:
            return []
        exact = self.find_exact(needle)
        out = []
        if exact is not None:
            out.append(exact)
        for e in self.find_prefix(needle, limit + 1):
            if e["id"] != (exact["id"] if exact else -1):
                out.append(e)
            if len(out) >= limit:
                break
        return out[:limit]

# --- partitions.csv ---------------------------------------------------------

def parse_partitions_csv(path: str) -> dict[str, dict]:
    """Parse firmware/partitions.csv into name -> {type, subtype, offset, size}."""
    out: dict[str, dict] = {}
    with open(path, "r", encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) < 5:
                raise ArchiveError(f"{path}:{lineno}: expected 5 columns")
            name, ptype, subtype = parts[0], parts[1].lower(), parts[2].lower()
            offset = int(parts[3], 0)
            size = int(parts[4], 0)
            if size < 0x1000:
                raise ArchiveError(f"{path}:{lineno}: partition {name} smaller than 4 KB")
            out[name] = dict(type=ptype, subtype=subtype, offset=offset, size=size)
    return out


def make_partition_image(data: bytes, size: int) -> bytes:
    if len(data) > size:
        raise ArchiveError(f"data {len(data)} bytes exceeds partition size {size}")
    return data + b"\xff" * (size - len(data))
