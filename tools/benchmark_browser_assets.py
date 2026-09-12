#!/usr/bin/env python3
"""Measure all shipped browser decoder files, including imported JS modules."""
import gzip,hashlib,json
from pathlib import Path
from pwpk_browser_server import INDEX_HTML
ROOT=Path(__file__).resolve().parents[1]
root=ROOT/'assets/pwpk-ui'
rows=[]
for p in sorted((root/'vendor').rglob('*')):
    if not p.is_file():continue
    data=p.read_bytes();rows.append(dict(asset=str(p.relative_to(root)),raw_bytes=len(data),gzip_bytes=len(gzip.compress(data,compresslevel=9,mtime=0)),sha256=hashlib.sha256(data).hexdigest()))
data=INDEX_HTML.encode();rows.append(dict(asset='prototype.html',raw_bytes=len(data),gzip_bytes=len(gzip.compress(data,compresslevel=9,mtime=0))))
result=dict(files=rows,total_raw_bytes=sum(r['raw_bytes'] for r in rows),total_gzip_bytes=sum(r['gzip_bytes'] for r in rows),note='Per-file gzip9 including WASM, JS imports, notices and prototype page; benchmark page omitted from shipping assets.')
output=ROOT/'benchmarks/browser_assets.json';output.parent.mkdir(exist_ok=True);output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
