import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
from pwpk import Article, build_pack
from pwpk_catalogue import Catalogue

def test_lazy_catalogue_lookup_and_roundtrip(tmp_path):
    paths=[]
    for n in range(3):
        p=tmp_path/f'p{n}.pwpk';p.write_bytes(build_pack([Article(n*10+1,'x',b'x'*20),Article(n*10+2,'y',b'y'*30)]));paths.append(p)
    c=Catalogue.from_files(paths); assert c.baseline_ram_bytes == sum(24 + len(str(p).encode()) for p in paths)
    saved=tmp_path/'catalog.json';c.save(saved); loaded=Catalogue.load(saved)
    ref,e,seeks=loaded.resolve(22);assert e.id==22 and seeks==3 and ref.path==str(paths[2])

def test_missing_scoped_article(tmp_path):
    p=tmp_path/'p.pwpk';p.write_bytes(build_pack([Article(1,'x',b'x')]))
    c=Catalogue.from_files([p])
    try:c.lookup(c.refs[0].pack_id,9)
    except KeyError:pass
    else:raise AssertionError('missing article resolved')
