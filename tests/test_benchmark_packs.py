import importlib.util
import sqlite3
import sys
from pathlib import Path
import numpy as np
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from pwpk import Article
from pwpk_corpus import read_corpus,statistics,corpus_hash,semantic_order,subset_indices,export
from benchmark_packs import metadata_experiment,cache_experiment,configurations,varint

def test_read_only_exact_export(tmp_path):
    path=tmp_path/'source.db'
    with sqlite3.connect(path) as c:
        c.executescript('CREATE TABLE articles(id INTEGER,title TEXT,status TEXT);CREATE TABLE minimax_distillations(article_id INTEGER,draft TEXT,status TEXT,quality_status TEXT);')
        c.execute('INSERT INTO articles VALUES(1,?,?)',('é','complete'))
        text='# é\r\n'+('content  \n'*20)
        c.execute('INSERT INTO minimax_distillations VALUES(1,?,?,?)',(text,'complete','complete'))
    before=path.read_bytes();a=read_corpus(path)
    assert a[0].text==text.encode() and path.read_bytes()==before
    export(a,tmp_path/'corpus.pwcr')
    assert (tmp_path/'corpus.pwcr').read_bytes().endswith(text.encode())
    assert statistics(a)['raw_bytes']==len(text.encode())
    assert corpus_hash(a)!=corpus_hash([Article(1,'e',a[0].text)])

def test_semantic_order_and_subsets_reproducible():
    a=[Article(i,'Biology' if i==0 else str(i),b'x') for i in range(100)]
    x=np.random.default_rng(11).normal(size=(100,768));x/=np.linalg.norm(x,axis=1,keepdims=True)
    ids=semantic_order(x)
    assert sorted(ids)==list(range(100)) and ids==semantic_order(x)
    assert list(subset_indices(a,x,25,'mixed'))==list(subset_indices(a,x,50,'mixed'))[:25]
    thematic=subset_indices(a,x,25,'thematic')
    assert thematic[0]==0
    assert (x[thematic]@x[0]).mean()>(x@x[0]).mean()

def test_metadata_and_cache_metrics():
    a=[Article(1,'abc',b'xy'),Article(2,'abd',b'z')]
    m=metadata_experiment(a)
    assert m['delta_id_bytes']==2 and m['id_u32_bytes']==8
    assert varint(127)==b'\x7f' and varint(128)==b'\x80\x01'
    rows=cache_experiment(a,{1:0,2:0})
    assert all(r['hit_rate']==0 for r in rows if r['blocks']==0)
    assert all(r['hit_rate']==.9999 for r in rows if r['blocks']>0)

def test_matrix_has_required_experiments():
    c=configurations()
    assert {r['level'] for r in c if r['codec']=='zstd'} >= {3,9,15,19,22}
    assert {r['dictionary_target'] for r in c}>={8192,16384,32768,65536}
    assert {r['block_size'] for r in c}>={16384,32768,65536,131072,262144}
    assert {r['codec'] for r in c}=={'zstd','brotli','xz','gzip'}
