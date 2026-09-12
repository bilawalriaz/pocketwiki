# Archive format

PocketWiki stores a library in `content.bin` and `index.bin`. The pack tool
creates these files. Firmware reads them from raw flash or from a `.pwp` file.
The built-in archive and optional packs use the same validated archive readers;
a failed optional-pack install never replaces the working built-in library.

All multi-byte values use little-endian byte order. Each article is compressed
independently, so firmware can read one article without decompressing others.

## `content.bin`

`content.bin` starts with `PWKC`. It has two supported versions.

| Version | Header size | Article codec | Dictionary |
| --- | ---: | --- | --- |
| 1 | 16 bytes | gzip | none |
| 2 | 32 bytes | zstd | optional shared dictionary |

Use version 1 for new C3-friendly packs with `--codec gzip`. The packer defaults
to version 2 when `--codec` is omitted. Firmware reads both versions.

### Version 1 header

```text
offset  size  field
0       4     magic: PWKC
4       1     version: 1
5       3     reserved: zero
8       4     header_size: 16
12      4     payload_size
16      ...   gzip article frames
```

### Version 2 header

```text
offset  size  field
0       4     magic: PWKC
4       1     version: 2
5       3     reserved: zero
8       4     header_size: 32
12      4     payload_size
16      4     dict_offset, relative to the payload
20      4     dict_len
24      8     reserved: zero
32      ...   zstd article frames, then an optional dictionary
```

For version 2 with a dictionary, `dict_offset` must equal
`payload_size - dict_len`. The dictionary is at the end of the payload. For a
pack without one, both values are zero.

## `index.bin`

`index.bin` starts with `PWKI`. Its 32-byte header describes a sequence of
40-byte article entries followed by UTF-8 title strings.

```text
offset  size  field
0       4     magic: PWKI
4       1     version: 1
5       1     entry_size: 40
6       2     reserved: zero
8       4     article_count
12      4     entries_offset: 32
16      4     strings_offset
20      4     index_size
24      4     CRC32 of bytes 32 through index_size
28      4     reserved: zero
```

Each entry has these fields:

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

Entries are sorted by normalized title bytes. IDs are sequential and match the
entry position. The firmware verifies bounds, order, IDs, and the index CRC.

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
4       1     version: 1
5       3     reserved: zero
8       4     content_len
12      4     index_len
16      4     CRC32 of content.bin + index.bin
20      12    reserved: zero
32      ...   content.bin
...     ...   index.bin
```

The device checks the outer CRC and length, then validates both embedded files.
It writes an upload to a temporary file and renames it only after validation.

## Build and verify

```sh
python3 tools/pack_content.py build /path/to/articles build/pack --codec gzip
python3 tools/pack_content.py verify build/pack
python3 tools/pack_content.py pack-file build/pack pack.pwp
```

Use `partition-image` only for the built-in flash archive. `.pwp` files are for
the internal pack store. See [Development](DEVELOPMENT.md) for build and flash
commands.
