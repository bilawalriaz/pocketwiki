#!/usr/bin/env python3
"""Build host streaming allocator probe and test real MiniMax compression units.

Measured host allocations are useful comparative evidence, not target SRAM or
code-size measurements. Dynamic library builds can differ from embedded ports.
"""
import argparse,json,subprocess,sys
from pathlib import Path
from benchmark_packs import train_dictionary
from pwpk_corpus import read_corpus,DEFAULT_DB
from pwpk import build_pack,Reader
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--db',type=Path,default=DEFAULT_DB);p.add_argument('--prefix',type=Path,default=Path('/opt/homebrew'));p.add_argument('--output',type=Path,default=ROOT/'benchmarks/decoder_memory.json');a=p.parse_args()
    directory=ROOT/'build/decoder-memory';directory.mkdir(parents=True,exist_ok=True);binary=directory/'probe'
    subprocess.run(['cc','-O2','-I'+str(a.prefix/'include'),str(ROOT/'benchmarks/decoder_memory.c'),'-L'+str(a.prefix/'lib'),'-lzstd','-lbrotlidec','-llzma','-lz','-o',str(binary)],check=True)
    records=read_corpus(a.db)[:1000];rows=[]
    for codec,level in [('zstd',19),('brotli',11),('xz',9),('gzip',9)]:
      for block in [0,16384,32768,65536,131072,262144]:
        for ds in ([0,8192,16384,32768,65536] if codec=='zstd' and block==0 else [0]):
          dictionary=train_dictionary(records,ds) if ds else b''
          reader=Reader(build_pack(records[:60],codec=codec,level=level,block_size=block,dictionary=dictionary))
          entry=reader.entries[0];offset,length=reader.compressed_range(entry.id);frame=directory/'frame.bin';frame.write_bytes(reader.data[offset:offset+length]);dpath=directory/'dictionary.bin';dpath.write_bytes(dictionary)
          result=subprocess.run([str(binary),codec,str(frame)]+([str(dpath)] if dictionary else []),check=True,capture_output=True,text=True)
          row=json.loads(result.stdout);row.update(level=level,block_target=block,dictionary_target=ds,compressed_bytes=length,platform=sys.platform,measurement='host allocator: excludes compressed input, allocator overhead, stack and external dictionary')
          rows.append(row);print(row,flush=True)
          a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(rows,indent=2)+'\n')
if __name__=='__main__':main()
