#!/usr/bin/env python3
"""Build reproducible browser-decoding fixtures and manifest.

Run this before opening the browser page; it never changes the source DB.
"""
from __future__ import annotations
import argparse, hashlib, json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import pwpk
from pwpk_corpus import read_corpus
import pack_content

def main(argv=None):
    ap=argparse.ArgumentParser(); ap.add_argument('--db',type=Path,required=True)
    ap.add_argument('--output',type=Path,default=Path('benchmarks/browser'))
    ap.add_argument('--count',type=int,default=100)
    a=ap.parse_args(argv); rec=read_corpus(a.db)[:a.count]
    a.output.mkdir(parents=True,exist_ok=True)
    samples=[x.text for x in rec]
    dictionary=pack_content.train_dict(samples, max_bytes=16384)
    variants=[('independent',0,b''),('dictionary',0,dictionary),('blocks-64k',65536,b'')]
    rows=[]
    for name,block,d in variants:
        data=pwpk.build_pack(rec,codec='zstd',level=9,block_size=block,dictionary=d)
        path=a.output/(name+'.pwpk');path.write_bytes(data)
        rows.append({'name':name,'path':str(path),'articles':len(rec),'pack_bytes':len(data),
                     'dictionary_bytes':len(d),'article_sha256':{str(x.id):hashlib.sha256(x.text).hexdigest() for x in rec}})
    out={'count':len(rec),'variants':rows,'command':'python tools/pwpk_browser_server.py benchmarks/browser/*.pwpk'}
    (a.output/'browser.json').write_text(json.dumps(out,indent=2)+'\n')
    print(json.dumps(out,indent=2))
if __name__=='__main__':main()
