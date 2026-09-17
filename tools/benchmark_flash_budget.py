#!/usr/bin/env python3
"""Calculate flash density and FAT+WL capacity using the installed IDF generator.

FAT geometry is generated on host, not read from a board. Device /api/stats
remains authoritative for its mounted volume. Density comes from measuring
tools/pack_content.py over the article domains in the pocketwiki-content
checkout. No flash writes occur.
"""
import argparse,json,math,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import content_paths

def partitions(path):
    out=[]
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith('#'):continue
        name,typ,sub,offset,size,*_=map(str.strip,line.split(','))
        out.append(dict(name=name,type=typ,subtype=sub,offset=int(offset,0),size=int(size,0)))
    return out

def production_density(corpus_dir: Path) -> dict:
    """Measure the shipping packer over the article domains.

    tools/pack_content.py is the only producer of packs, so measuring it over
    the db-packs domains is what the budget artifact should carry: bytes per
    article and whole-pack totals for the format the firmware actually reads.
    """
    import archive_format as af
    import pack_content
    packs = sorted(d for d in corpus_dir.iterdir() if d.is_dir())
    if not packs:
        return None
    stats = []
    for pack_dir in packs:
        with tempfile.TemporaryDirectory(prefix="pw-budget-") as tmp:
            out = Path(tmp) / "out"
            manifest = pack_content.build(str(pack_dir), str(out))
            blob = af.pack_pack_file((out / "content.bin").read_bytes(),
                                     (out / "index.bin").read_bytes())
        stats.append(dict(articles=manifest["article_count"],
                          payload=manifest["compressed_total"],
                          dict=manifest["dict_bytes"],
                          pack_bytes=len(blob)))
    total_articles = sum(s["articles"] for s in stats)
    total_payload = sum(s["payload"] for s in stats)
    average_pack = sum(s["pack_bytes"] for s in stats) / len(stats)
    return dict(codec="deflate", level=pack_content.DEFLATE_LEVEL,
                dictionary_target=pack_content.DICT_MAX_BYTES, dictionary_scope="pack",
                ordering="source", block_size=0, n=round(total_articles / len(stats)),
                composition="db-packs", bytes_per_article=total_payload / total_articles,
                total_pack_bytes=average_pack, packs=len(stats),
                corpus_bytes=total_payload + sum(s["dict"] for s in stats))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--idf',type=Path,default=Path.home()/'.platformio/packages/framework-espidf');p.add_argument('--production-corpus',type=Path,default=content_paths.ARTICLES/'db-packs',help='measure the shipping packer over these article domains');a=p.parse_args()
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
                         firmware_image_bytes=binary.stat().st_size if binary.exists() else None,
                         geometry_note='Host IDF WLFATFS generated geometry, 4096-byte sectors/clusters, two FATs, 512 root entries; confirm actual mounted volume.',
                         worst_case_cluster_slack={str(n):n*4095 for n in [1,5,10,25]}))
    produced = production_density(a.production_corpus) if a.production_corpus.is_dir() else None
    density = []
    if produced is not None:
        per_pack = produced["n"]
        for row in rows:
            budget = row["usable_pack_budget"]
            whole_packs = budget // produced["total_pack_bytes"]
            density.append(dict(
                case="production-v3", n=per_pack, composition=produced["composition"],
                codec=produced["codec"], level=produced["level"],
                dictionary_target=produced["dictionary_target"],
                dictionary_scope=produced["dictionary_scope"],
                block_size=0, ordering=produced["ordering"], budget_bytes=budget,
                bytes_per_article=round(produced["bytes_per_article"], 1),
                estimated_articles=int(budget / produced["bytes_per_article"]),
                whole_packs_fit=whole_packs,
                whole_pack_articles=whole_packs * per_pack,
                note="Measured with tools/pack_content.py over "
                     f"{produced['packs']} article domains of {per_pack} articles each."))
    output = dict(layouts=rows, density=density, production=produced)
    (ROOT/'benchmarks'/'flash_budget.json').write_text(json.dumps(output,indent=2)+'\n')
    print(json.dumps(rows,indent=2))
if __name__=='__main__':main()
