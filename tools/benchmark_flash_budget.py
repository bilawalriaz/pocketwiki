#!/usr/bin/env python3
"""Calculate flash density and FAT+WL capacity using the installed IDF generator.

FAT geometry is generated on host, not read from a board. Device /api/stats
remains authoritative for its mounted volume. No flash writes occur here.
"""
import argparse,json,math,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def partitions(path):
    out=[]
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith('#'):continue
        name,typ,sub,offset,size,*_=map(str.strip,line.split(','))
        out.append(dict(name=name,type=typ,subtype=sub,offset=int(offset,0),size=int(size,0)))
    return out

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--idf',type=Path,default=Path.home()/'.platformio/packages/framework-espidf');p.add_argument('--results',type=Path,default=ROOT/'benchmarks/results.json');p.add_argument('--browser-gzip-bytes',type=int,default=0);a=p.parse_args()
    sys.path.insert(0,str(a.idf/'components/fatfs'))
    try:from wl_fatfsgen import WLFATFS
    except ImportError:raise SystemExit('Install construct: python3 -m pip install --target build/bench-deps construct==2.10.70; run with PYTHONPATH=build/bench-deps')
    rows=[]
    for target,file in [('esp32-c3','partitions.csv'),('esp32-s3','partitions_16mb.csv')]:
        layout=partitions(ROOT/'firmware'/file);size=next(x['size'] for x in layout if x['name']=='packs')
        fs=WLFATFS(size=size,sector_size=4096,sectors_per_cluster=1,fat_tables_cnt=2,root_entry_count=512)
        s=fs.plain_fatfs.state.boot_sector_state
        data=(s.sectors_count-s.reserved_sectors_cnt-s.fat_tables_cnt*s.sectors_per_fat_cnt-s.root_dir_sectors_cnt)*4096
        binary=ROOT/'firmware/dist'/target/'firmware.bin'
        if not binary.exists():binary=ROOT/'firmware/.pio/build'/target/'firmware.bin'
        rows.append(dict(target=target,layout=layout,nominal_flash_bytes=max(x['offset']+x['size'] for x in layout),pack_partition_bytes=size,
                         generated_fat_usable_bytes=data,wl_bytes=size-fs.plain_fat_sectors*4096,fat_metadata_bytes=fs.plain_fat_sectors*4096-data,
                         filesystem_overhead_percent=(size-data)/size*100,reserved_free_bytes=8192,usable_pack_budget=data-8192,
                         firmware_image_bytes=binary.stat().st_size if binary.exists() else None,browser_decoder_gzip_bytes=a.browser_gzip_bytes,
                         browser_note='Additional browser asset must fit inside app partition if embedded; not subtracted twice from packs.',
                         geometry_note='Host IDF WLFATFS generated geometry, 4096-byte sectors/clusters, two FATs, 512 root entries; confirm actual mounted volume.',
                         worst_case_cluster_slack={str(n):n*4095 for n in [1,5,10,25]}))
    source=json.loads(a.results.read_text());corpus=json.loads((a.results.parent/'corpus.json').read_text());density=[]
    for r in source:
        if r.get('status')!='ok':continue
        if r['n'] not in [100,1000,5000,corpus['count']]:continue
        # Charge dictionaries, titles, index and header by repeating the measured
        # user-pack pattern. No fractional dictionary subsidy.
        for budget in [m*1024**2 for m in [2,4,8,12,14,16]]+[x['usable_pack_budget'] for x in rows]:
            estimated=min(corpus['count'],int(budget/r['bytes_per_article']))
            density.append(dict(case=r['key'],n=r['n'],composition=r['composition'],codec=r['codec'],level=r['level'],dictionary_target=r['dictionary_target'],dictionary_scope=r['dictionary_scope'],block_size=r['block_size'],ordering=r['ordering'],budget_bytes=budget,
                                estimated_articles=estimated,corpus_percent=estimated/corpus['count']*100,
                                whole_packs_fit=budget//r['total_pack_bytes'],whole_pack_articles=min(corpus['count'],(budget//r['total_pack_bytes'])*r['n']),
                                note='Average-density estimate; whole-pack count additionally shown. Real selection and final partial pack need rebuilding.'))
    output=dict(layouts=rows,density=density);(a.results.parent/'flash_budget.json').write_text(json.dumps(output,indent=2)+'\n')
    print(json.dumps(rows,indent=2))
if __name__=='__main__':main()
