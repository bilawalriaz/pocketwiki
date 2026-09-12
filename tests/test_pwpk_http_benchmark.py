import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from benchmark_pwpk_http import scenario_items,percentile
from pwpk import Article,Reader,build_pack

def test_scenarios_respect_unit_and_pack_boundaries():
    a=[Article(i,str(i),b'x'*100) for i in range(12)]
    packs={'a':Reader(build_pack(a,block_size=400)),'b':Reader(build_pack(a[3:]))}
    same=scenario_items(packs,'same-block',30)
    assert len({e.unit_offset for _,e in same})==1 and len({e.id for _,e in same})>1
    assert len({e.unit_offset for _,e in scenario_items(packs,'different-block',30)})>1
    assert len({name for name,_ in scenario_items(packs,'different-packs',30)})==2
    assert scenario_items(packs,'random',30)==scenario_items(packs,'random',30)
    assert percentile(list(range(1,101)),.95)==95
    assert percentile([],.95) is None
