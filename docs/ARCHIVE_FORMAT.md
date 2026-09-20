# Archive format

PocketWiki stores a library in `content.bin` and `index.bin`. The pack tool
creates these files. Firmware reads them from raw flash or from a `.pwp` file.
The built-in archive and optional packs use the same validated archive readers;
a failed optional-pack install never replaces the working built-in library.

All multi-byte values are little-endian. Each article is compressed
independently, so firmware reads one article without touching the others.

## `content.bin`

`content.bin` starts with `PWKC` and has one format version, 3:

| Version | Header size | Article codec | Dictionary |
| --- | ---: | --- | --- |
| 3 | 32 bytes | raw DEFLATE | shared trained dictionary |

Every article is an independent DEFLATE stream (RFC 1951), compressed at zlib
level 9 with `memLevel` 9 against that dictionary. It is a *raw* stream: no
zlib or gzip wrapper, because the dictionary is supplied out of band.

The dictionary is trained on the pack's own articles and stored at the end of
the payload, so per-article offsets are unaffected by its presence. The device
reads it into the 32 KiB inflate ring the article decoder already owns,
right-aligned, so a back-reference of distance `d` at ring position `p` reads
`ring[(p - d) mod 32768]`, which is the `d`-th byte from the end of the
dictionary, and then inflates the stream out of that same ring. No second
buffer, no allocation, no per-article heap.

That is what makes dictionary compression affordable on the 4 MB board: the
ESP32-C3 keeps Bluetooth resident for the Android app, and this design adds
nothing to its static memory. Measured on the repository's 2,209-article
corpus (16,018,944 raw bytes, 7,251.7 B/article, per-article independent
streams), the format stores **2,189 bytes per article** against 3,418 for
plain gzip, and 2,388 B/article for the per-domain packs that ship, which is
825 articles in the C3's pack store and 6,022 in the S3's.

### Limits

| Limit | Value | Where |
| --- | ---: | --- |
| dictionary | ≤ 32 KiB | `PW_MAX_DICT_BYTES` |
| back-reference distance | ≤ 32 KiB | DEFLATE itself; the ring is the window |
| article body | ≤ 2 MiB | `MAX_ARTICLE_BYTES` (packer) |

`tools/pack_content.py` trains a dictionary of at most 32 KiB (`DICT_MAX_BYTES`,
sized down for small corpora) and the device refuses a larger one when the pack
is offered, before install rather than per article.

### Header

```text
offset  size  field
0       4     magic: PWKC
4       1     version: 3
5       3     reserved: zero
8       4     header_size: 32
12      4     payload_size
16      4     dict_offset, relative to the payload
20      4     dict_len
24      8     reserved: zero
32      ...   DEFLATE article streams, then the dictionary
```

`dict_offset` must equal `payload_size - dict_len`; without a dictionary, both
values are zero.

## `index.bin`

`index.bin` starts with `PWKI`. Its 32-byte header describes a sequence of
40-byte article entries followed by UTF-8 title strings.

```text
offset  size  field
0       4     magic: PWKI
4       1     version: 3
5       1     entry_size: 40
6       2     reserved: zero
8       4     article_count
12      4     entries_offset: 32
16      4     strings_offset
20      4     index_size
24      4     CRC32 of bytes 32 through index_size
28      4     reserved: zero
```

Each entry:

```text
offset  size  field
0       4     id
4       4     content_offset, relative to the content payload
8       4     comp_len
12      4     uncomp_len
16      4     CRC32 of uncompressed article bytes
20      4     norm_off, absolute offset in index.bin
24      2     norm_len
26      4     disp_off, absolute offset in index.bin
30      2     disp_len
32      8     reserved: zero
```

The index version must match the content version. Entries are sorted by
normalized title bytes; IDs are sequential and match the entry position. The
firmware verifies bounds, order, IDs, and the index CRC, and the index CRC plus
`uncomp_len` are the whole integrity contract for a served article: a v3 frame
carries no checksum of its own, so a wrong dictionary or a truncated stream
fails the CRC instead of serving plausible-looking text.

## Title normalization

Python and C must produce the same bytes. The algorithm works on UTF-8 bytes:

1. Remove leading and trailing ASCII whitespace.
2. Replace each internal run of ASCII whitespace with one space.
3. Convert ASCII `A` through `Z` to lowercase.
4. Copy all non-ASCII bytes unchanged.

Do not add Unicode case folding. It would break cross-language lookup parity.

## Portable `.pwp` file

A `.pwp` file wraps both archive files for upload.

```text
offset  size  field
0       4     magic: PWKP
4       1     version: 3
5       3     reserved, zero
8       4     content_len
12      4     index_len
16      4     CRC32 of content.bin + index.bin
20      12    reserved, zero
32      ...   content.bin
...     ...   index.bin
```

The device checks the outer CRC and length, then validates both embedded files.
It writes an upload to a temporary file and renames it only after validation.

## Authoring articles

The packer accepts Markdown or HTML. Markdown is a documented subset: ATX
headings, paragraphs, flat lists, pipe tables, fenced code, blockquotes,
horizontal rules, `[text](target)` links, bold, italic, and inline code.

Two rules catch people out:

- **Raw HTML in Markdown is escaped**, so `<a href="...">text</a>` renders as
  literal text. Write `[text](target)` instead.
- **External links are dropped**: the href disappears and the text stays.
  Articles are self-contained by design; write an off-device address as inline
  code, as this guide does for `packs.educated.space`.

A link target resolves to an article in the same pack by its file name without
the extension (`[First setup](00002-first-setup)`), and the packer rewrites it
to the device's `/a/<id>` route. A target that does not resolve keeps its text
and loses the link.

## Build and verify

```sh
python3 tools/pack_content.py build /path/to/articles build/pack
python3 tools/pack_content.py verify build/pack
python3 tools/pack_content.py pack-file build/pack pack.pwp
```

Use `partition-image` only for the built-in flash archive. `.pwp` files are for
the internal pack store. See [Development](DEVELOPMENT.md) for build and flash
commands.
