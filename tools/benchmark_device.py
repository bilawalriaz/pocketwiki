#!/usr/bin/env python3
"""Run reproducible HTTP load tests against a PocketWiki board.

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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://192.168.4.1", help="board base URL")
    parser.add_argument("--target", choices=("esp32c3", "esp32s3"), default="esp32c3")
    parser.add_argument("--port", help="optional serial port for read-only chip-id")
    parser.add_argument("--clients", default="1,2,5,10,20", help="comma-separated concurrency levels")
    parser.add_argument("--requests", type=int, default=20, help="requests per case")
    parser.add_argument("--article", action="append", type=int, help="article id (repeatable)")
    parser.add_argument("--pack", default="", help="pack query name")
    parser.add_argument("--mode", choices=("decoded",), default="decoded",
                        help="decoded article reads, the only serving path the firmware has")
    parser.add_argument("--scenario", choices=("same", "random", "fixed"), default="same",
                        help="same popular ID, deterministic random IDs, or cycle --article IDs")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--output", default="benchmarks/device-results.json")
    parser.add_argument("--seed", type=int, default=271828)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    ids = args.article or [0]
    query = ("?pack=" + urllib.parse.quote(args.pack)) if args.pack else ""
    prefix = "/a/"
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
