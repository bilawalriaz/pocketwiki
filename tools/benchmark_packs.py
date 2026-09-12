#!/usr/bin/env python3
"""Reproducible PWPK size/round-trip/random-access benchmark (read-only source).

Install tools/requirements-benchmarks.txt, then use --quick or --full.
Completed cases are content-addressed and resumed; --rerun refreshes timings.
"""
from __future__ import annotations
import argparse
import csv
import gzip
import hashlib
import importlib.metadata
import json
import lzma
import platform
import random
import statistics as stats
import sys
import time
import unicodedata
from pathlib import Path
import brotli
import numpy as np
import zstandard as zstd
from pwpk_corpus import (DEFAULT_DB,DEFAULT_EMBEDDINGS,SEED,read_corpus,statistics,
                          embeddings,subset_indices,ordered,corpus_hash)
from pwpk import build_pack,Reader
ROOT=Path(__file__).resolve().parents[1]
VERSION=1

def configurations(quick=False,suites=None):
    suites=set(suites or ['baseline','dictionary','blocks','codecs'])
    rows=[]
    def add(codec='zstd',level=19,block=0,order='source',dictionary=0,scope='none'):
        rows.append(dict(codec=codec,level=level,block_size=block,ordering=order,dictionary_target=dictionary,dictionary_scope=scope))
    if 'baseline' in suites:
        for level in ([3,19,22] if quick else [3,9,15,19,22]):add(level=level)
    if 'dictionary' in suites:
        for size in ([8192,32768] if quick else [8192,16384,32768,65536]):
            add(dictionary=size,scope='pack')
            add(dictionary=size,scope='global')
    if 'blocks' in suites:
        for size in ([32768,65536] if quick else [16384,32768,65536,131072,262144]):
            for order in ['source','alphabetical','random','semantic']:add(block=size,order=order)
    if 'codecs' in suites:
        for codec,level in [('gzip',9),('brotli',11),('xz',9)]:
            add(codec,level)
            add(codec,level,65536,'semantic')
    return rows

def train_dictionary(records,size):
    # Trained from this installed pack for density, not a held-out generalization claim.
    # Feed fragments when tiny packs otherwise provide too few training samples.
    samples=[a.text[i:i+1024] for a in records for i in range(0,len(a.text),1024)]
    return zstd.train_dictionary(size,samples).as_bytes()

def varint(n):
    out=bytearray()
    while n>=128:out.append((n&127)|128);n>>=7
    out.append(n);return bytes(out)

def metadata_experiment(records):
    previous=0;ids=bytearray();lengths=bytearray();titles=bytearray();prev=b''
    for a in sorted(records,key=lambda a:a.id):ids+=varint(a.id-previous);previous=a.id;lengths+=varint(len(a.text))
    for a in sorted(records,key=lambda a:a.title.encode()):
        t=a.title.encode();prefix=0
        while prefix<min(len(prev),len(t)) and prev[prefix]==t[prefix]:prefix+=1
        titles+=varint(prefix)+varint(len(t)-prefix)+t[prefix:];prev=t
    raw=b''.join(varint(len(a.title.encode()))+a.title.encode() for a in records)
    return dict(id_u32_bytes=4*len(records),delta_id_bytes=len(ids),raw_length_u32_bytes=4*len(records),varint_length_bytes=len(lengths),
                length_prefixed_titles_bytes=len(raw),front_coded_titles_bytes=len(titles),zstd_titles_bytes=len(zstd.ZstdCompressor(level=19).compress(raw)),
                note='isolated encodings, not a replacement index; restart/offset tables required for random access are not included')

def normalization_experiment(records):
    transforms={'crlf':lambda s:s.replace('\r\n','\n'),'trailing_whitespace':lambda s:'\n'.join(x.rstrip(' \t') for x in s.split('\n')),
                'blank_lines':lambda s:__import__('re').sub(r'\n{3,}','\n\n',s),'nfc':lambda s:unicodedata.normalize('NFC',s)}
    compressor=zstd.ZstdCompressor(level=19)
    before_raw=sum(len(a.text) for a in records);before_comp=sum(len(compressor.compress(a.text)) for a in records)
    rows=[]
    for name,fn in transforms.items():
        raw=comp=changed=0
        for a in records:
            data=fn(a.text.decode('utf-8')).encode('utf-8');raw+=len(data);comp+=len(compressor.compress(data));changed+=data!=a.text
        rows.append(dict(transform=name,changed_articles=changed,raw_bytes=raw,raw_saved=before_raw-raw,compressed_payload_bytes=comp,compressed_saved=before_comp-comp))
    return rows

def cache_experiment(records,unit_for):
    rng=random.Random(SEED);n=len(records);popular=list(range(min(10,n)));units=[unit_for[a.id] for a in records]
    streams={'random':[rng.randrange(n) for _ in range(10000)],
             'popular':[rng.choice(popular) if rng.random()<.8 else rng.randrange(n) for _ in range(10000)],
             'related':[(start+j)%n for start in [rng.randrange(n) for _ in range(1000)] for j in range(10)]}
    rows=[]
    for name,requests in streams.items():
        for capacity in [0,1,2,4]:
            cache=[];hits=0
            for i in requests:
                u=units[i]
                if u in cache: hits+=1;cache.remove(u)
                cache.append(u)
                if len(cache)>capacity:cache.pop(0)
            rows.append(dict(traffic=name,blocks=capacity,hit_rate=hits/len(requests),requests=len(requests)))
    return rows

def measure(records,config,dictionary):
    start=time.perf_counter();blob=build_pack(records,codec=config['codec'],level=config['level'],block_size=config['block_size'],dictionary=dictionary)
    encode_s=time.perf_counter()-start
    reader=Reader(blob)
    # Every article is validated, including every article in every mini-block.
    start=time.perf_counter()
    for a in records:
        if reader.extract(a.id)!=a.text:raise AssertionError('round-trip mismatch')
    random_access_decode_s=time.perf_counter()-start
    m=reader.metrics
    entries=reader.entries
    decoded=sum(e.unit_raw_length for e in entries)
    reads=sum(e.compressed_length for e in entries)
    raw=sum(len(a.text) for a in records)
    row=dict(raw_bytes=raw,payload_bytes=m['payload_bytes'],index_bytes=m['index_bytes'],title_bytes=m['title_bytes'],dictionary_bytes=len(dictionary),
             container_bytes=m['header_bytes'],total_pack_bytes=len(blob),effective_ratio=raw/len(blob),bytes_per_article=len(blob)/len(records),
             compressed_bytes_read=reads,decoded_bytes_processed=decoded,unwanted_bytes_decoded=decoded-raw,read_amplification=decoded/raw,
             compressed_read_ratio=reads/raw,encode_seconds=encode_s,random_access_decode_seconds=random_access_decode_s,
             host_decode_mib_s=decoded/max(random_access_decode_s,1e-9)/(1024**2),roundtrip_articles=len(records))
    # Actual unit decoding throughput excludes repeated block decode and lookup validation.
    unique={e.unit_offset:e for e in entries};start=time.perf_counter()
    for e in unique.values():reader.extract(e.id)
    row['host_unique_decode_mib_s']=sum(e.unit_raw_length for e in unique.values())/max(time.perf_counter()-start,1e-9)/(1024**2)
    row['max_unit_raw_bytes']=max(e.unit_raw_length for e in entries)
    row['unit_count']=len(unique)
    return row,blob,{e.id:e.unit_offset for e in entries}

def write_report(rows,corpus,path):
    ok=[r for r in rows if r.get('status')=='ok']
    lines=['# Compression benchmark','',f"Source: `{corpus['source_column']}`; {corpus['count']:,} articles; {corpus['raw_bytes']:,} exact UTF-8 bytes.",
           f"Mean {corpus['mean']:.1f}; median {corpus['median']:.1f}; p90 {corpus['p90']:.1f}; maximum {corpus['max']:,} bytes.",'',
           'All successful rows round-trip every article byte-for-byte. Timings are host measurements, not ESP estimates. Ratio = raw / complete PWPK bytes. Read amplification = decoded unit bytes / requested article bytes. Global dictionary bytes are charged in full to every pack (standalone deployment).', '',
           'The full row matrix, component sizes, timings, hashes and failures are in `benchmarks/results.csv` and `benchmarks/results.json`. This table selects the smallest total pack in each family per composition and N.','',
           '| Strategy | Composition | N | Codec/level | Dict bytes | Block target | Ordering | Raw bytes | Payload | Index | Titles+header | Total | Ratio | Amplification | Host MiB/s |',
           '|---|---|---:|---|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|']
    groups={}
    for r in ok:
        family='dictionary-'+r['dictionary_scope'] if r['dictionary_target'] else ('blocks' if r['block_size'] else 'independent')
        key=(r['composition'],r['n'],r['codec'],family)
        if key not in groups or r['total_pack_bytes']<groups[key]['total_pack_bytes']:groups[key]=r
    for key,r in sorted(groups.items()):
        lines.append(f"| {key[3]} | {r['composition']} | {r['n']} | {r['codec']}/{r['level']} | {r['dictionary_bytes']} | {r['block_size']} | {r['ordering']} | {r['raw_bytes']} | {r['payload_bytes']} | {r['index_bytes']} | {r['title_bytes']+r['container_bytes']} | {r['total_pack_bytes']} | {r['effective_ratio']:.3f} | {r['read_amplification']:.2f} | {r['host_decode_mib_s']:.1f} |")
    lines+=['','## Semantic ordering: measured bytes saved versus source order','', '| Composition | N | Block | Source bytes | Semantic bytes | Saved bytes | Saved % |','|---|---:|---:|---:|---:|---:|---:|']
    for r in ok:
        if r['codec']=='zstd' and r['ordering']=='semantic' and r['block_size']:
            base=next((s for s in ok if s['n']==r['n'] and s['composition']==r['composition'] and s['codec']==r['codec'] and s['level']==r['level'] and s['block_size']==r['block_size'] and s['ordering']=='source'),None)
            if base:
                saved=base['total_pack_bytes']-r['total_pack_bytes'];lines.append(f"| {r['composition']} | {r['n']} | {r['block_size']} | {base['total_pack_bytes']} | {r['total_pack_bytes']} | {saved} | {saved/base['total_pack_bytes']*100:.2f} |")
    path.write_text('\n'.join(lines)+'\n')

def main():
    p=argparse.ArgumentParser(description=__doc__);mode=p.add_mutually_exclusive_group();mode.add_argument('--quick',action='store_true');mode.add_argument('--full',action='store_true')
    p.add_argument('--db',type=Path,default=DEFAULT_DB);p.add_argument('--embeddings',type=Path,default=DEFAULT_EMBEDDINGS)
    p.add_argument('--sizes',help='comma-separated counts or full');p.add_argument('--suites',default='baseline,dictionary,blocks,codecs')
    p.add_argument('--compositions',default='mixed,thematic');p.add_argument('--output',type=Path,default=ROOT/'benchmarks')
    p.add_argument('--keep-packs',action='store_true');p.add_argument('--rerun',action='store_true');p.add_argument('--normalization',action='store_true')
    a=p.parse_args();out=a.output;out.mkdir(parents=True,exist_ok=True);runs=out/'runs';runs.mkdir(exist_ok=True)
    corpus=read_corpus(a.db);s=statistics(corpus);x=embeddings(corpus,a.embeddings)
    s.update(seed=SEED,embedding_sha256=hashlib.sha256(x.tobytes()).hexdigest(),embedding_count=len(x),embedding_dimensions=x.shape[1],
             environment=dict(python=sys.version,platform=platform.platform(),versions={k:importlib.metadata.version(k) for k in ['zstandard','brotli','numpy']}))
    (out/'corpus.json').write_text(json.dumps(s,indent=2)+'\n');print(json.dumps(s),flush=True)
    implementation=hashlib.sha256(Path(__file__).read_bytes()+Path(__file__).with_name('pwpk.py').read_bytes()+Path(__file__).with_name('pwpk_corpus.py').read_bytes()).hexdigest()
    sizes=a.sizes.split(',') if a.sizes else (['25','100','500','1000'] if a.quick else ['25','50','100','250','500','1000','2500','5000','full'])
    configs=configurations(a.quick,a.suites.split(','));rows=[];global_dicts={}
    # Stable 10% global training pool: global costs counted per pack. This is a
    # deployment density experiment; do not interpret as unseen-corpus accuracy.
    train_ids=np.random.default_rng(SEED+1).permutation(len(corpus))[:min(2500,len(corpus))]
    for ntext in sizes:
      n=len(corpus) if ntext=='full' else int(ntext)
      for composition in a.compositions.split(','):
        if n==len(corpus) and composition!='mixed':continue
        ids=subset_indices(corpus,x,n,composition);selected=[corpus[i] for i in ids];vectors=x[ids]
        shash=corpus_hash(sorted(selected,key=lambda a:a.id));order_cache={o:ordered(selected,vectors,o) for o in ['source','alphabetical','random','semantic']}
        md=metadata_experiment(selected);(runs/f'metadata-{composition}-{n}.json').write_text(json.dumps(md,indent=2)+'\n')
        dictionaries={}
        for config in configs:
          identity=dict(version=VERSION,libraries=s['environment']['versions'],corpus=s['sha256'],embedding_sha256=s['embedding_sha256'],selection=shash,n=n,composition=composition,**config)
          # Include implementation bytes in resume key: fixes invalidate cached measurements.
          identity['implementation']=implementation
          key=hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest()[:20];result_path=runs/(key+'.json')
          if result_path.exists() and not a.rerun:rows.append(json.loads(result_path.read_text()));continue
          row=dict(identity,key=key,status='ok');start=time.perf_counter()
          try:
            target=config['dictionary_target'];scope=config['dictionary_scope'];dictionary=b''
            if target:
              cache=global_dicts if scope=='global' else dictionaries
              if target not in cache:cache[target]=train_dictionary([corpus[i] for i in train_ids] if scope=='global' else selected,target)
              dictionary=cache[target]
            measured,blob,unit_for=measure(order_cache[config['ordering']],config,dictionary);row.update(measured)
            if a.keep_packs:(runs/(key+'.pwpk')).write_bytes(blob)
            if config['block_size']==65536 and config['ordering']=='semantic' and config['codec']=='zstd':
              (runs/f'cache-{composition}-{n}.json').write_text(json.dumps(cache_experiment(order_cache['semantic'],unit_for),indent=2)+'\n')
          except Exception as exc:row.update(status='error',error=f'{type(exc).__name__}: {exc}')
          row['case_seconds']=time.perf_counter()-start;result_path.write_text(json.dumps(row,indent=2)+'\n');rows.append(row)
          print(f"{composition} {n} {config} -> {row.get('total_pack_bytes',row.get('error'))} ({row['case_seconds']:.2f}s)",flush=True)
          # Preserve consolidated output after every case, even when interrupted.
          write_results(out,rows,s)
    if a.normalization:(out/'normalization.json').write_text(json.dumps(normalization_experiment(corpus),indent=2)+'\n')
    write_results(out,rows,s)

def write_results(out,rows,s):
    # Include earlier successful matrices from the same corpus and implementation.
    identities={(r['key']) for r in rows};all_rows=list(rows)
    current_impl=rows[-1]['implementation'] if rows else None
    for path in (out/'runs').glob('*.json'):
        r=json.loads(path.read_text())
        if isinstance(r,dict) and r.get('implementation')==current_impl and r.get('corpus')==s['sha256'] and r.get('key') not in identities:all_rows.append(r);identities.add(r['key'])
    all_rows.sort(key=lambda r:(r['n'],r['composition'],r['codec'],r['block_size'],r['dictionary_target'],r['level'],r['ordering'],r['dictionary_scope']))
    (out/'results.json').write_text(json.dumps(all_rows,indent=2)+'\n')
    keys=sorted(set().union(*(r.keys() for r in all_rows))) if all_rows else []
    with (out/'results.csv').open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=keys);w.writeheader();w.writerows(all_rows)
    write_report(all_rows,s,out/'COMPRESSION_BENCHMARK.md')
if __name__=='__main__':main()
