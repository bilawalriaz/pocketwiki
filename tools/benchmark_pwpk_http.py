#!/usr/bin/env python3
"""Install validated PWPK fixtures and exercise the experimental device service.

Example: python3 tools/benchmark_pwpk_http.py --url http://192.168.4.1 \
 --packs biology=build/biology.pwpk computing=build/computing.pwpk --install
Host concurrency is bounded independently of the board's one decoder worker.
"""
import argparse,concurrent.futures,hashlib,json,random,statistics,time,urllib.request,urllib.error
from pathlib import Path
from pwpk import Reader
from pwpk_corpus import SEED

def fetch(url,data=None,timeout=15):
    start=time.perf_counter()
    try:
        with urllib.request.urlopen(urllib.request.Request(url,data=data),timeout=timeout) as r:return r.status,r.read(),time.perf_counter()-start,None
    except urllib.error.HTTPError as e:return e.code,e.read(),time.perf_counter()-start,str(e)
    except Exception as e:return 0,b'',time.perf_counter()-start,f'{type(e).__name__}: {e}'
def telemetry(base):
    status,body,_,err=fetch(base+'/pwpk/packs')
    try:return json.loads(body) if status==200 else dict(status=status,error=err)
    except ValueError:return dict(status=status,error='invalid telemetry')
def percentile(values,p):
    if not values:return None
    values=sorted(values);return values[min(len(values)-1,max(0,__import__('math').ceil(p*len(values))-1))]
def scenario_items(packs,scenario,count,seed=SEED):
    rng=random.Random(seed);items=[(name,e) for name,r in packs.items() for e in r.entries]
    first=next(iter(packs));r=packs[first]
    if scenario=='same':return [items[0]]*count
    if scenario=='same-block':
        unit=r.entries[0].unit_offset;group=[(first,e) for e in r.entries if e.unit_offset==unit]
        if len(group)<2:raise ValueError('same-block requires a mini-block pack with at least two articles')
    elif scenario=='different-block':
        seen=set();group=[]
        for e in r.entries:
            if e.unit_offset not in seen:group.append((first,e));seen.add(e.unit_offset)
        if len(group)<2:raise ValueError('different-block requires at least two blocks')
    elif scenario=='different-packs':
        if len(packs)<2:raise ValueError('different-packs requires two packs')
        group=[(name,reader.entries[0]) for name,reader in packs.items()]
    elif scenario=='popular':return [rng.choice(items[:min(5,len(items))]) if rng.random()<.8 else rng.choice(items) for _ in range(count)]
    elif scenario=='random':return [rng.choice(items) for _ in range(count)]
    else:raise ValueError('unknown scenario')
    return [group[i%len(group)] for i in range(count)]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--url',default='http://192.168.4.1');p.add_argument('--packs',nargs='+',required=True,help='name=local.pwpk')
    p.add_argument('--install',action='store_true');p.add_argument('--clients',default='1,2,5,10,20');p.add_argument('--buffers',default='1024,2048,4096,8192,16384');p.add_argument('--modes',default='raw,decoded')
    p.add_argument('--scenarios',default='random,same,same-block,different-block,different-packs,popular');p.add_argument('--requests',type=int,default=100);p.add_argument('--timeout',type=float,default=15)
    p.add_argument('--output',type=Path,default=Path('benchmarks/device_http.json'));a=p.parse_args();base=a.url.rstrip('/');packs={}
    for spec in a.packs:
        name,path=spec.split('=',1)
        if not name or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in name):raise ValueError('invalid pack name')
        blob=Path(path).read_bytes();packs[name]=Reader(blob)
        if a.install:
            status,body,_,error=fetch(base+'/pwpk/'+name,data=blob,timeout=120)
            if status!=200:raise RuntimeError(f'install {name}: {status} {body!r} {error}')
    # Reference hashes computed outside measured request loops.
    expected={}
    for name,r in packs.items():
        for e in r.entries:
            off,size=r.compressed_range(e.id)
            expected[name,e.id,'raw']=hashlib.sha256(r.data[off:off+size]).digest()
            expected[name,e.id,'decoded']=hashlib.sha256(r.extract(e.id)).digest()
    results=[];report=dict(url=base,seed=SEED,packs={n:dict(pack_id=f'{r.pack_id:016x}',articles=r.count) for n,r in packs.items()},cases=results,
                          note='Latency includes body receipt/hash validation; failures retained, successful latency reported separately. Serial logs required for watchdog/reboot confirmation.')
    a.output.parent.mkdir(parents=True,exist_ok=True)
    for scenario in a.scenarios.split(','):
        try:items=scenario_items(packs,scenario,a.requests)
        except ValueError as e:results.append(dict(scenario=scenario,status='skipped',reason=str(e)));continue
        for mode in a.modes.split(','):
            if mode not in ['raw','decoded']:raise ValueError(mode)
            for buffer in map(int,a.buffers.split(',')):
                if buffer not in [1024,2048,4096,8192,16384]:raise ValueError(buffer)
                for clients in map(int,a.clients.split(',')):
                    if clients<1:raise ValueError(clients)
                    before=telemetry(base);start=time.perf_counter()
                    def one(item):
                        name,e=item;status,body,elapsed,error=fetch(f'{base}/pwpk/{name}/{"raw" if mode=="raw" else "article"}/{e.id}?buffer={buffer}',timeout=a.timeout)
                        valid=status==200 and hashlib.sha256(body).digest()==expected[name,e.id,mode]
                        return dict(status=status,valid=valid,bytes=len(body),latency_ms=elapsed*1000,error=error)
                    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:requests=list(pool.map(one,items))
                    elapsed=time.perf_counter()-start;after=telemetry(base);success=[r for r in requests if r['valid']];lat=[r['latency_ms'] for r in success]
                    row=dict(scenario=scenario,mode=mode,buffer_bytes=buffer,concurrency=clients,requests=len(requests),successes=len(success),failures=len(requests)-len(success),
                             median_latency_ms=statistics.median(lat) if lat else None,p95_latency_ms=percentile(lat,.95),p99_latency_ms=percentile(lat,.99),
                             successful_requests_per_second=len(success)/elapsed,successful_bytes_per_second=sum(r['bytes'] for r in success)/elapsed,elapsed_seconds=elapsed,before=before,after=after,
                             errors=[r for r in requests if not r['valid']],stability='requires serial log correlation')
                    results.append(row);a.output.write_text(json.dumps(report,indent=2)+'\n');print({k:v for k,v in row.items() if k not in ['before','after','errors']},flush=True)
if __name__=='__main__':main()
