#!/usr/bin/env python3
"""Run reproducible HTTP/PWPK load tests against a PocketWiki board.

The script deliberately uses only the Python standard library so it can run on
the host used to flash the board.  It does not flash or erase anything.  A
board is expected to be reachable at --url (the default is the SoftAP address).
"""
from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import os
import random
import statistics
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
import pathlib


def get(url: str, timeout: float) -> tuple[int, bytes, float]:
    started = time.perf_counter()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            body = response.read()
            return response.status, body, time.perf_counter() - started
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read(), time.perf_counter() - started
    except Exception as exc:  # serialisable result for load reports
        return 0, str(exc).encode(), time.perf_counter() - started


def stats(base: str, timeout: float) -> dict:
    code, body, _ = get(base.rstrip("/") + "/api/stats", timeout)
    if code != 200:
        return {"status": code, "error": body.decode(errors="replace")}
    try:
        result = json.loads(body)
        result["status"] = code
        return result
    except json.JSONDecodeError:
        return {"status": code, "error": "invalid JSON", "body": body[:200].decode(errors="replace")}


def chip_info(port: str, chip: str) -> dict:
    """Read chip/flash identity; this command has no write operation."""
    command = ["esptool", "--port", port, "--chip", chip, "chip-id"]
    try:
        proc = subprocess.run(command, check=False, text=True, capture_output=True, timeout=20)
        return {"command": command, "returncode": proc.returncode,
                "output": (proc.stdout + proc.stderr)[-4000:]}
    except Exception as exc:
        return {"command": command, "error": str(exc)}


def one_request(url: str, timeout: float) -> dict:
    status, body, elapsed = get(url, timeout)
    return {"status": status, "bytes": len(body), "latency_ms": elapsed * 1000}


def run_case(base: str, path: str | list[str], clients: int, requests: int, timeout: float) -> dict:
    paths = [path] if isinstance(path, str) else path
    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:
        results = list(pool.map(lambda i: one_request(base.rstrip("/") + paths[i % len(paths)], timeout), range(requests)))
    elapsed = time.perf_counter() - started
    latencies = sorted(r["latency_ms"] for r in results)
    successes = sum(r["status"] == 200 for r in results)
    return {
        "path": paths[0] if len(paths) == 1 else "<varying>", "concurrency": clients, "requests": requests,
        "successes": successes, "failures": requests - successes,
        "median_latency_ms": statistics.median(latencies) if latencies else None,
        "p95_latency_ms": latencies[max(0, int(len(latencies) * .95) - 1)] if latencies else None,
        "p99_latency_ms": latencies[max(0, int(len(latencies) * .99) - 1)] if latencies else None,
        "requests_per_second": requests / elapsed if elapsed else 0,
        "bytes": sum(r["bytes"] for r in results),
        "bytes_per_second": sum(r["bytes"] for r in results) / elapsed if elapsed else 0,
    }


def serial_benchmark(args) -> int:
    """Generate real units; temporarily configure/build, app-only flash, capture."""
    import hashlib, re, sys, zlib
    import serial
    import zstandard as zstd
    from pwpk_corpus import read_corpus, DEFAULT_DB, corpus_hash
    from benchmark_packs import train_dictionary
    from pwpk import build_pack, Reader
    root=pathlib.Path(__file__).resolve().parents[1];fw=root/'firmware'
    if not args.port:raise ValueError('--port is required for serial benchmark')
    records=read_corpus(DEFAULT_DB)[:100]
    profiles=[]
    for level in [3,19]:profiles.append((f'zstd-l{level}',records[:1],level,0,b''))
    for size in [8192,32768]:profiles.append((f'zstd-dict-{size}',records[:1],19,0,train_dictionary(records,size)))
    for size in [16384,32768,65536]:profiles.append((f'zstd-block-{size}',records,19,size,b''))
    c=['#include "pwpk_serial_benchmark.h"'];rows=[];manifest=[]
    for i,(name,selection,level,block,dictionary) in enumerate(profiles):
        r=Reader(build_pack(selection,level=level,block_size=block,dictionary=dictionary));e=r.entries[0];off,n=r.compressed_range(e.id);frame=r.data[off:off+n]
        raw=zstd.ZstdDecompressor(dict_data=zstd.ZstdCompressionDict(dictionary) if dictionary else None).decompress(frame)
        c.append(f'static const uint8_t frame_{i}[]={{'+','.join(map(str,frame))+'};')
        if dictionary:c.append(f'static const uint8_t dict_{i}[]={{'+','.join(map(str,dictionary))+'};')
        rows.append(f'{{"{name}",frame_{i},sizeof frame_{i},{len(raw)},'+(f'dict_{i},sizeof dict_{i}' if dictionary else 'NULL,0')+f',{zlib.crc32(raw)&0xffffffff}u}}')
        manifest.append(dict(profile=name,raw_bytes=len(raw),frame_bytes=len(frame),dictionary_bytes=len(dictionary),raw_sha256=hashlib.sha256(raw).hexdigest()))
    c.append('const pwpk_serial_fixture_t pwpk_serial_fixtures[]={'+','.join(rows)+'};')
    c.append('const size_t pwpk_serial_fixture_count=sizeof pwpk_serial_fixtures/sizeof pwpk_serial_fixtures[0];')
    fixture=fw/'main/pwpk_serial_fixture.c';fixture.write_text('\n'.join(c)+'\n')
    env='esp32-c3' if args.target=='esp32c3' else 'esp32-s3'
    configs=[fw/'sdkconfig.defaults',fw/f'sdkconfig.{env}'];saved={p:p.read_bytes() if p.exists() else None for p in configs}
    image=fw/'dist'/env/'firmware.bin'
    try:
        for p in configs:
            text=p.read_text() if p.exists() else ''
            text=re.sub(r'^(?:# )?CONFIG_POCKETWIKI_(?:SERIAL_BENCHMARK|PWPK_SERVICE)(?:=.*| is not set)$','',text,flags=re.M)
            p.write_text(text+'\nCONFIG_POCKETWIKI_SERIAL_BENCHMARK=y\nCONFIG_POCKETWIKI_PWPK_SERVICE=y\n')
        subprocess.run(['pio','run','-d',str(fw),'-e',env],check=True)
        info=chip_info(args.port,args.target)
        backup=root/'build/device-backup'/env/'app-before.bin';backup.parent.mkdir(parents=True,exist_ok=True)
        if not backup.exists():subprocess.run(['esptool','--port',args.port,'--chip',args.target,'read-flash','0x10000','0x180000',str(backup)],check=True)
        subprocess.run(['esptool','--port',args.port,'--chip',args.target,'write-flash','0x10000',str(image)],check=True)
        raw_log=bytearray();deadline=time.monotonic()+45
        with serial.Serial(args.port,115200,timeout=.5) as ser:
            ser.dtr=False
            while time.monotonic()<deadline:
                raw_log.extend(ser.read(4096))
                if b'"benchmark_complete":true' in raw_log:break
        log=raw_log.decode(errors='replace');measurements=[]
        for line in log.splitlines():
            if 'pwpk_bench:' in line and '{' in line:
                try:measurements.append(json.loads(line[line.index('{'):]))
                except ValueError:pass
        out=pathlib.Path(args.output);out.parent.mkdir(parents=True,exist_ok=True);out.with_suffix('.log').write_text(log)
        report=dict(target=args.target,fixture_corpus_sha256=corpus_hash(records),profiles=manifest,firmware_bytes=image.stat().st_size,firmware_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),chip_info=info,app_backup=str(backup),configuration=dict(PWPK_SERVICE=True,SERIAL_BENCHMARK=True),measurements=measurements)
        out.write_text(json.dumps(report,indent=2)+'\n')
        if not any(r.get('benchmark_complete') for r in measurements):raise RuntimeError('incomplete serial capture; raw log saved')
        print(out);return 0
    finally:
        for p,data in saved.items():
            if data is None:p.unlink(missing_ok=True)
            else:p.write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://192.168.4.1", help="board base URL")
    parser.add_argument("--target", choices=("esp32c3", "esp32s3"), default="esp32c3")
    parser.add_argument("--port", help="optional serial port for read-only chip-id")
    parser.add_argument("--clients", default="1,2,5,10,20", help="comma-separated concurrency levels")
    parser.add_argument("--requests", type=int, default=20, help="requests per case")
    parser.add_argument("--article", action="append", type=int, help="article id (repeatable)")
    parser.add_argument("--pack", default="", help="pack query name")
    parser.add_argument("--mode", choices=("decoded", "raw"), default="decoded")
    parser.add_argument("--scenario", choices=("same", "random", "fixed"), default="same",
                        help="same popular ID, deterministic random IDs, or cycle --article IDs")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--output", default="benchmarks/device-results.json")
    parser.add_argument("--seed", type=int, default=271828)
    parser.add_argument("--serial-benchmark", action="store_true",
                        help="generate a corpus fixture, build/flash app only, and capture JSON serial telemetry")
    args = parser.parse_args()
    if args.serial_benchmark:
        return serial_benchmark(args)
    rng = random.Random(args.seed)
    ids = args.article or [0]
    query = ("?pack=" + urllib.parse.quote(args.pack)) if args.pack else ""
    prefix = "/a/" if args.mode == "decoded" else "/raw/a/"
    before_stats=stats(args.url,args.timeout)
    cases = []
    for clients_text in args.clients.split(","):
        clients = int(clients_text)
        if args.scenario == "random":
            selected = [prefix + str(rng.randrange(0, 100)) + query for _ in range(args.requests)]
        elif args.scenario == "fixed":
            selected = [prefix + str(article_id) + query for article_id in ids]
        else:
            selected = prefix + str(ids[0]) + query
        cases.append(run_case(args.url, selected,
                                  clients, args.requests, args.timeout))
    result = {"target": args.target, "url": args.url, "mode": args.mode,
              "scenario": args.scenario,
              "seed": args.seed, "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "before": before_stats, "cases": cases,
              "after": stats(args.url, args.timeout)}
    if args.port:
        result["chip_info"] = chip_info(args.port, args.target)
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as fh:
        json.dump(result, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
