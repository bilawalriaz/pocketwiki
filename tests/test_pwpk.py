import sys
import struct
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
import pwpk


def articles():
    return [pwpk.Article(30, "Zeta", b"z" * 80),
            pwpk.Article(10, "Alpha", b"alpha\n" * 20),
            pwpk.Article(20, "Beta", b"beta\n" * 30)]


def test_independent_codecs_round_trip_and_sorted_index():
    for codec in pwpk.CODECS:
        blob = pwpk.build_pack(articles(), codec=codec, level=9)
        reader = pwpk.Reader(blob)
        assert [e.id for e in reader.entries] == [10, 20, 30]
        assert [reader.extract(a.id) for a in articles()] == [a.text for a in articles()]
        assert reader.title(10) == "Alpha"
        off, size = reader.compressed_range(20)
        assert blob[off:off + size]


def test_solid_units_extract_exact_slices():
    reader = pwpk.Reader(pwpk.build_pack(articles(), block_size=200))
    assert reader.stats().unit_count < len(articles())
    for a in articles():
        assert reader.extract(a.id) == a.text


def test_dictionary_round_trip_and_stats():
    dictionary = b"alpha beta gamma " * 100
    reader = pwpk.Reader(pwpk.build_pack(articles(), dictionary=dictionary))
    assert reader.dictionary == dictionary
    assert reader.stats().dictionary_bytes == len(dictionary)
    assert reader.extract(10) == articles()[1].text


def test_corruption_and_truncation_rejected():
    blob = bytearray(pwpk.build_pack(articles()))
    blob[-1] ^= 1
    try:
        pwpk.Reader(bytes(blob))
    except ValueError:
        pass
    else:
        raise AssertionError("CRC corruption accepted")
    try:
        pwpk.Reader(bytes(blob[:-1]))
    except ValueError:
        pass
    else:
        raise AssertionError("truncation accepted")


def test_deterministic_and_duplicate_ids_rejected():
    assert pwpk.build_pack(articles()) == pwpk.build_pack(articles())
    try:
        pwpk.build_pack([pwpk.Article(1, "a", b"x"), pwpk.Article(1, "b", b"y")])
    except ValueError:
        pass
    else:
        raise AssertionError("duplicate ID accepted")


def test_bounds_rejected_even_when_outer_crc_is_recomputed():
    blob = bytearray(pwpk.build_pack(articles()))
    # First entry's raw offset (entry offset 16) points beyond its unit.
    struct.pack_into("<I", blob, pwpk.HEADER_SIZE + 16, 0xFFFFFFFF)
    struct.pack_into("<I", blob, 88, zlib.crc32(blob[pwpk.HEADER_SIZE:]) & 0xffffffff)
    try:
        pwpk.Reader(bytes(blob))
    except ValueError:
        pass
    else:
        raise AssertionError("out-of-bounds entry accepted")


def _changed(blob,offset,fmt,value):
    out=bytearray(blob);struct.pack_into(fmt,out,offset,value)
    struct.pack_into('<I',out,88,zlib.crc32(out[96:])&0xffffffff)
    return bytes(out)


def test_structural_mutations_and_resource_limits():
    import pytest
    blob=pwpk.build_pack(articles(),block_size=4096)
    for offset,fmt,value in [(92,'I',1),(56,'Q',1),(96+40,'I',2**30),
                             (96+20,'I',1),(96+44,'I',1),(96+16,'I',1)]:
        with pytest.raises(ValueError):pwpk.Reader(_changed(blob,offset,'<'+fmt,value))
    with pytest.raises(ValueError):pwpk.build_pack([])
    with pytest.raises(ValueError):pwpk.build_pack([pwpk.Article(1,'x',b'')])
    with pytest.raises(ValueError):pwpk.build_pack(articles(),block_size=-1)


def test_each_codec_refuses_trailing_frames_and_decode_size_mismatch():
    import pytest
    raw=b'educational article '*50
    for codec in pwpk.CODECS:
        encoded=pwpk._compress(raw,codec,9,b'')
        assert pwpk._decompress(encoded,codec,b'',len(raw))==raw
        with pytest.raises(Exception):pwpk._decompress(encoded+encoded,codec,b'',len(raw))
        with pytest.raises(Exception):pwpk._decompress(encoded,codec,b'',10)


def test_size_accounting_and_request_amplification():
    from benchmark_packs import measure
    source=articles();config={'codec':'zstd','level':9,'block_size':4096}
    row,blob,units=measure(source,config,b'')
    assert row['total_pack_bytes']==sum(row[k] for k in ['payload_bytes','index_bytes','title_bytes','dictionary_bytes','container_bytes'])
    assert row['read_amplification']==len(source)
    assert row['unwanted_bytes_decoded']==sum(len(a.text) for a in source)*(len(source)-1)
    assert len(set(units.values()))==1
    assert row['effective_ratio']==sum(len(a.text) for a in source)/len(blob)
