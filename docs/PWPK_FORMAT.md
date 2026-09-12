# PWPK v1

PWPK is the single-file experimental pack used by the host compression and
serving experiments. All integers are little-endian. The writer is
`tools/pwpk.py`; the format intentionally has no filesystem or SQLite
dependency.

The 96-byte header is:

| offset | size | field |
|---:|---:|---|
| 0 | 4 | magic `PWPK` |
| 4 | 1 | version (`1`) |
| 5 | 1 | codec (`1` zstd, `2` Brotli, `3` XZ/LZMA2, `4` gzip) |
| 6 | 2 | flags, currently zero |
| 8 | 4 | header size (`96`) |
| 12 | 4 | article count |
| 16 | 8 | deterministic pack identifier |
| 24 | 8 | index offset (always 96) |
| 32 | 8 | index length |
| 40 | 8 | title table offset |
| 48 | 8 | title table length |
| 56 | 8 | dictionary offset (zero when absent) |
| 64 | 8 | dictionary length |
| 72 | 8 | compressed payload offset |
| 80 | 8 | compressed payload length |
| 88 | 4 | CRC32 of index, titles, dictionary, and payload |
| 92 | 4 | reserved, zero |

The index has one 48-byte entry per article, sorted by numeric ID. Its fields
are `id u64`, compression-unit offset and length (`u64,u32`, relative to the
payload), article offset and uncompressed length (`u32,u32`, relative to the
decoded unit), title offset and length (`u32,u16`, relative to the title
table), a reserved `u16`, article CRC32 (`u32`), decoded unit length (`u32`),
and four trailing reserved bytes. Thus lookup can return the exact compressed range and
the slice to use after decoding one unit.

Articles are physically supplied in the order passed to `build_pack`. With a
zero block target every article is an independent frame. A nonzero target
groups whole articles until the target would be exceeded; an article larger
than the target remains a unit by itself. The index remains ID-sorted, so
physical ordering can be semantic without changing logical lookup.

The title table stores UTF-8 display titles with duplicate strings shared.
An optional dictionary is stored between titles and payload and is currently
supported for Zstandard only. The CRC is checked before entries are exposed;
all offsets, lengths, sorted IDs, and article CRCs are checked by `Reader`.
Truncation, trailing bytes, overlap beyond a section, bad codec/version, and
corrupt decompressed articles raise `ValueError`.

Example:

```python
from tools.pwpk import Article, Reader, build_pack
blob = build_pack([Article(7, "Gravity", b"...")], block_size=0)
assert Reader(blob).extract(7) == b"..."
```
