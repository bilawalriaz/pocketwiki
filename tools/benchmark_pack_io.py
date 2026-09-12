#!/usr/bin/env python3
"""Host cached-file I/O; never label these rates as ESP flash throughput.

With no --packs, create 25 disjoint real-corpus packs under ignored build/.
"""
import argparse,json,random,time,tracemalloc
from pathlib import Path
from pwpk_catalogue import Catalogue,FileReader
from pwpk import build_pack
from pwpk_corpus import read_corpus,DEFAULT_DB,SEED
ROOT=Path(__file__).resolve().parents[1]
def run(paths,samples=100):
    cat=Catalogue.from_files(paths);rows=[];scale=[];rng=random.Random(SEED)
    # IDs sampled from on-disk fixed entries, outside timed/peak regions.
    from pwpk import _ENTRY,HEADER_SIZE,ENTRY_SIZE
    ids={}
    for ref in cat.refs:
        with open(ref.path,'rb') as f:
            f.seek(HEADER_SIZE);ids[ref.pack_id]=[_ENTRY.unpack(f.read(ENTRY_SIZE))[0] for _ in range(ref.article_count)]
    for n in [1,5,10,25]:
        if len(cat.refs)<n:continue
        tracemalloc.start();c=Catalogue(cat.refs[:n]);baseline=tracemalloc.get_traced_memory()[0];started=time.perf_counter();reads=0
        for _ in range(samples):
            ref=rng.choice(c.refs);reader=FileReader(ref.path,validate_body=False);reader.lookup(rng.choice(ids[ref.pack_id]));reads+=reader.last_entry_reads
        elapsed=time.perf_counter()-started;current,peak=tracemalloc.get_traced_memory();tracemalloc.stop()
        scale.append(dict(packs=n,lookups=samples,elapsed_seconds=elapsed,index_entry_reads=reads,mean_entry_reads=reads/samples,
                          catalogue_incremental_python_bytes=baseline,lookup_peak_python_bytes=peak,
                          metadata_and_path_model_bytes=c.baseline_ram_bytes,note='CPython incremental traced allocations; PackRef objects pre-exist. Not ESP RAM.'))
    for ref in cat.refs:
        reader=FileReader(ref.path,validate_body=False);chosen=[rng.choice(ids[ref.pack_id]) for _ in range(samples)]
        ranges=[reader.lookup(i) for i in chosen]
        for size in [1024,2048,4096,8192,16384]:
            started=time.perf_counter();total=calls=0
            with open(ref.path,'rb',buffering=0) as stream:
                for e in ranges:
                    stream.seek(reader.payload_off+e.unit_offset);left=e.compressed_size
                    while left:
                        chunk=stream.read(min(size,left))
                        if not chunk:raise ValueError('truncated stream')
                        total+=len(chunk);left-=len(chunk);calls+=1
            elapsed=time.perf_counter()-started
            rows.append(dict(pack_id=ref.pack_id,buffer_bytes=size,samples=samples,bytes_read=total,read_calls=calls,elapsed_seconds=elapsed,mib_s=total/elapsed/1048576))
    return dict(catalogue_packs=len(cat.refs),scale=scale,rows=rows,measurement='host cached files; no device, HTTP or raw-flash comparison implied',
                catalogue_formula='24-byte conceptual fixed metadata plus UTF-8 path bytes per pack; excludes allocator overhead',
                seek_model='96-byte header plus measured binary-search entry reads; validation CRC only at catalogue creation')
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--packs',nargs='+',type=Path);p.add_argument('--db',type=Path,default=DEFAULT_DB);p.add_argument('--output',type=Path,default=ROOT/'benchmarks/pack_io.json');p.add_argument('--samples',type=int,default=100);a=p.parse_args()
    if not a.packs:
        from pwpk_corpus import embeddings,ordered
        records=read_corpus(a.db);directory=ROOT/'build/pack-io';directory.mkdir(parents=True,exist_ok=True);a.packs=[]
        for n in range(25):
            path=directory/f'pack-{n}.pwpk';path.write_bytes(build_pack(records[n*100:(n+1)*100],level=19,block_size=65536));a.packs.append(path)
    result=run(a.packs,a.samples);a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result['scale'],indent=2))
if __name__=='__main__':main()
