#!/usr/bin/env python3
"""Build a byte-preserving experimental pack from the read-only MiniMax corpus.

Auto dictionary choice measures complete candidate packs for this exact user
selection; it does not extrapolate a threshold from one topic.
"""
import argparse,json
from pathlib import Path
from pwpk import build_pack,Reader
from pwpk_corpus import DEFAULT_DB,DEFAULT_EMBEDDINGS,read_corpus,embeddings,subset_indices,ordered,corpus_hash
from benchmark_packs import train_dictionary

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--db',type=Path,default=DEFAULT_DB);p.add_argument('--embeddings',type=Path,default=DEFAULT_EMBEDDINGS)
    p.add_argument('--size',type=int,default=1000);p.add_argument('--ids',help='comma-separated explicit article IDs overrides size/composition')
    p.add_argument('--composition',default='mixed');p.add_argument('--ordering',choices=['source','alphabetical','random','semantic'],default='source')
    p.add_argument('--codec',choices=['zstd','gzip','brotli','xz'],default='zstd');p.add_argument('--level',type=int,default=19);p.add_argument('--block-size',type=int,default=0)
    p.add_argument('--dictionary',default='auto',help='auto, none, or bytes (8192,16384,32768,65536)');p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    all_records=read_corpus(a.db);x=embeddings(all_records,a.embeddings)
    if a.ids:
        wanted={int(i) for i in a.ids.split(',')};indices=[i for i,r in enumerate(all_records) if r.id in wanted]
        if len(indices)!=len(wanted):raise ValueError('some requested IDs are missing')
    else:indices=subset_indices(all_records,x,a.size,a.composition)
    records=[all_records[i] for i in indices];records=ordered(records,x[indices],a.ordering)
    sizes=([0,8192,16384,32768,65536] if a.dictionary=='auto' and a.codec=='zstd' and not a.block_size else [0]) if a.dictionary in ['auto','none'] else [int(a.dictionary)]
    best=None;trials=[]
    for size in sizes:
        try:
            dictionary=train_dictionary(records,size) if size else b''
            blob=build_pack(records,codec=a.codec,level=a.level,block_size=a.block_size,dictionary=dictionary)
            trials.append(dict(dictionary_bytes=len(dictionary),pack_bytes=len(blob)))
            if best is None or len(blob)<len(best):best=blob
        except Exception as exc:
            if a.dictionary!='auto':raise
            trials.append(dict(dictionary_target=size,error=str(exc)))
    if best is None:raise ValueError('no valid candidate')
    reader=Reader(best)
    for article in records:
        if reader.extract(article.id)!=article.text:raise AssertionError('roundtrip failed')
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_bytes(best)
    result=dict(path=str(a.output),selection_sha256=corpus_hash(records),pack_id=f'{reader.pack_id:016x}',articles=len(records),codec=a.codec,level=a.level,ordering=a.ordering,block_size=a.block_size,metrics=reader.metrics,trials=trials)
    a.output.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
