"""Read-only MiniMax corpus extraction and deterministic embedding-based selection.

No JSON serialization is included in payload measurements. The binary export is
length-prefixed UTF-8, preserving each source draft byte for byte.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import sqlite3
import struct
from pathlib import Path
import numpy as np

DEFAULT_DB = Path.home() / 'wiki-distill' / 'educational-source.db'
DEFAULT_EMBEDDINGS = DEFAULT_DB.with_name('educational-embeddings.db')
SEED = 20260909
QUERY = '''SELECT a.id, a.title, m.draft FROM articles a
JOIN minimax_distillations m ON m.article_id=a.id
WHERE a.status='complete' AND m.status='complete' AND m.quality_status='complete'
AND length(trim(COALESCE(m.draft,'')))>100 ORDER BY a.id'''

def read_corpus(path=DEFAULT_DB):
    from pwpk import Article
    with sqlite3.connect(Path(path).resolve().as_uri()+'?mode=ro', uri=True) as db:
        db.execute('PRAGMA query_only=ON')
        return [Article(int(i), t, s.encode('utf-8')) for i,t,s in db.execute(QUERY)]

def corpus_hash(records):
    h=hashlib.sha256()
    for a in records:
        title=a.title.encode('utf-8')
        h.update(struct.pack('<III',a.id,len(title),len(a.text)))
        h.update(title);h.update(a.text)
    return h.hexdigest()

def statistics(records):
    sizes=np.array([len(a.text) for a in records],dtype=np.int64)
    return dict(count=len(records),raw_bytes=int(sizes.sum()),mean=float(sizes.mean()),
                median=float(np.median(sizes)),p90=float(np.percentile(sizes,90)),max=int(sizes.max()),
                sha256=corpus_hash(records),source_column='minimax_distillations.draft',selection_sql=QUERY)

def export(records,path):
    with Path(path).open('wb') as f:
        f.write(b'PWCR'+struct.pack('<II',1,len(records)))
        for a in records:
            t=a.title.encode('utf-8');f.write(struct.pack('<III',a.id,len(t),len(a.text)));f.write(t);f.write(a.text)

def embeddings(records,path=DEFAULT_EMBEDDINGS):
    """Use existing float32 little endian vectors; never call an embedding model."""
    rows={}
    with sqlite3.connect(Path(path).resolve().as_uri()+'?mode=ro',uri=True) as db:
        db.execute('PRAGMA query_only=ON')
        for i,d,v in db.execute("SELECT article_id,dimensions,vector FROM embeddings WHERE status='complete'"):
            if d!=768 or len(v)!=d*4: raise ValueError('invalid embedding dimensions')
            rows[i]=np.frombuffer(v,dtype='<f4')
    missing=[a.id for a in records if a.id not in rows]
    if missing: raise ValueError(f'Missing existing embeddings for {len(missing)} articles')
    x=np.stack([rows[a.id] for a in records]);norm=np.linalg.norm(x,axis=1,keepdims=True)
    if not np.isfinite(x).all() or (norm==0).any():raise ValueError('invalid embedding')
    return x/norm

def subset_indices(records,x,n,composition):
    if n>len(records):raise ValueError('subset exceeds corpus')
    if composition=='mixed':return np.random.default_rng(SEED).permutation(len(records))[:n]
    # A reproducible anchor; measure coherence from its cosine neighbours.
    target='Biology' if composition=='thematic' else composition.removeprefix('topic:')
    anchor=next((i for i,a in enumerate(records) if a.title==target),None)
    if anchor is None:raise ValueError('topic anchor missing: '+target)
    return np.argsort(-(x@x[anchor]),kind='stable')[:n]

def semantic_order(x):
    """Balanced recursive projection splits; O(N log N), no ML dependency.

    Split along the line between a deterministic point and its farthest point.
    Recursively keep nearest halves contiguous; source position breaks ties.
    """
    def split(ids):
        if len(ids)<=8:return ids.tolist()
        v=x[ids];a=v[0];b=v[np.argmin(v@a)];direction=a-b
        order=np.argsort(-(v@direction),kind='stable');s=ids[order];mid=len(s)//2
        return split(s[:mid])+split(s[mid:])
    return split(np.arange(len(x)))

def ordered(records,x,ordering):
    if ordering=='source':ids=sorted(range(len(records)),key=lambda i:records[i].id)
    elif ordering=='alphabetical':ids=sorted(range(len(records)),key=lambda i:records[i].title.encode())
    elif ordering=='random':ids=np.random.default_rng(SEED).permutation(len(records))
    elif ordering=='semantic':ids=semantic_order(x)
    else:raise ValueError(ordering)
    return [records[i] for i in ids]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--db',type=Path,default=DEFAULT_DB);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();records=read_corpus(a.db);a.output.parent.mkdir(parents=True,exist_ok=True);export(records,a.output)
    a.output.with_suffix('.stats.json').write_text(json.dumps(statistics(records),indent=2)+'\n')
if __name__=='__main__':main()
