"""Small file-resident catalogue for installed PWPK files.

Only fixed metadata and paths are retained. Pack indexes are opened lazily for
one lookup and are never loaded into a process-wide catalogue.
"""
from __future__ import annotations
import json
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path
from pwpk import _HEADER, _ENTRY, HEADER_SIZE, ENTRY_SIZE, CODEC_NAMES, Entry


@dataclass(frozen=True)
class PackRef:
    pack_id: int
    path: str
    article_count: int
    size: int


class FileReader:
    """Header/index-only reader; each lookup reads O(log article_count) entries."""
    def __init__(self, path, validate_body=True):
        self.path = Path(path)
        self.size = self.path.stat().st_size
        with self.path.open('rb') as f:
            h=f.read(HEADER_SIZE)
            if len(h)!=HEADER_SIZE: raise ValueError('truncated PWPK header')
            (magic,ver,cid,flags,hs,count,pack_id,index_off,index_len,titles_off,titles_len,dict_off,dict_len,payload_off,payload_len,crc,res)=_HEADER.unpack(h)
            if magic!=b'PWPK' or ver!=1 or flags or hs!=HEADER_SIZE or cid not in CODEC_NAMES or res or (dict_len and cid != 1): raise ValueError('invalid PWPK header')
            if index_off!=HEADER_SIZE or index_len!=count*ENTRY_SIZE or titles_off!=index_off+index_len or payload_off!=titles_off+titles_len+dict_len or payload_off+payload_len!=self.size or (dict_len and dict_off!=titles_off+titles_len) or (not dict_len and dict_off): raise ValueError('invalid PWPK offsets')
            got=0
            f.seek(index_off)
            left=self.size-index_off if validate_body else 0
            while left:
                b=f.read(min(65536,left));
                if not b: raise ValueError('truncated PWPK body')
                got=zlib.crc32(b,got);left-=len(b)
            if validate_body and (got&0xffffffff)!=crc: raise ValueError('PWPK CRC mismatch')
        self.count=count;self.pack_id=pack_id;self.payload_off=payload_off;self.payload_len=payload_len;self.titles_off=titles_off;self.titles_len=titles_len

    def lookup(self, article_id):
        self.last_entry_reads=0
        lo,hi=0,self.count-1
        with self.path.open('rb') as f:
            while lo<=hi:
                mid=(lo+hi)//2;f.seek(HEADER_SIZE+mid*ENTRY_SIZE);b=f.read(ENTRY_SIZE)
                if len(b)!=ENTRY_SIZE: raise ValueError('truncated PWPK entry')
                self.last_entry_reads+=1
                eid,uo,cs,ro,rs,to,ts,reserved,crc,unit_raw,pad=_ENTRY.unpack(b)
                if reserved or pad!=b'\0'*4 or uo+cs>self.payload_len or ro+rs>unit_raw or to+ts>self.titles_len: raise ValueError('invalid PWPK entry')
                if eid==article_id:return Entry(eid,uo,cs,ro,rs,to,ts,crc,unit_raw)
                if eid<article_id:lo=mid+1
                else:hi=mid-1
        raise KeyError(article_id)


class Catalogue:
    def __init__(self, refs):
        self.refs = tuple(refs)
        self._by_id = {r.pack_id: r for r in self.refs}
        if len(self._by_id) != len(self.refs): raise ValueError("duplicate pack ID")

    @classmethod
    def from_files(cls, paths):
        refs = []
        for path in paths:
            p = Path(path)
            reader = FileReader(p)
            refs.append(PackRef(reader.pack_id, str(p), reader.count, p.stat().st_size))
        return cls(refs)

    def save(self, path):
        Path(path).write_text(json.dumps([r.__dict__ for r in self.refs], sort_keys=True) + "\n")

    @classmethod
    def load(cls, path):
        return cls(PackRef(**x) for x in json.loads(Path(path).read_text()))

    def lookup(self, pack_id, article_id):
        """Return (pack ref, entry), opening only the selected pack."""
        ref = self._by_id[pack_id]
        reader=FileReader(ref.path,validate_body=False)
        if reader.pack_id != ref.pack_id or reader.size != ref.size:raise ValueError('installed pack changed; refresh catalogue')
        return ref, reader.lookup(article_id)

    def resolve(self, article_id):
        """Find an ID across packs; returns the first match and seek count."""
        seeks = 0
        for ref in self.refs:
            seeks += 1
            try: return (*self.lookup(ref.pack_id, article_id), seeks)
            except KeyError: continue
        raise KeyError(article_id)

    @property
    def baseline_ram_bytes(self):
        # Pack metadata plus UTF-8 path bytes. This is a wire/model estimate,
        # not CPython object or allocator usage.
        return sum(24 + len(r.path.encode()) for r in self.refs)
