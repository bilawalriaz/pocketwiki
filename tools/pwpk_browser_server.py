#!/usr/bin/env python3
"""Range-serving browser prototype for PWPK archives.

The device-side equivalent only needs to implement the same byte-range route:
the browser fetches a zstd frame (and, for dictionary packs, the dictionary
once), then decodes it locally.  This server deliberately never decompresses
an article while serving it.
"""
from __future__ import annotations

import argparse
import http.server
import json
import hashlib
import os
import sys
import urllib.parse
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import archive_format as af  # noqa: E402
import pwpk  # noqa: E402


class PackSet:
    def __init__(self, paths):
        self.packs = {}
        for p in paths:
            path = Path(p)
            if path.is_dir():
                content = path / "content.bin"
                index = path / "index.bin"
                name = path.name
                if not (content.exists() and index.exists()):
                    continue
                raw_content, raw_index = content.read_bytes(), index.read_bytes()
                self.packs[name] = af.Archive(raw_content, raw_index)
                continue
            else:
                name = path.stem
                data = path.read_bytes()
                try:
                    self.packs[name] = pwpk.Reader(data)
                except ValueError:
                    raw_content, raw_index = af.unpack_pack_file(data)
                    self.packs[name] = af.Archive(raw_content, raw_index)
        if not self.packs:
            raise ValueError("no content.bin/index.bin directories or .pwp files found")

    def manifest(self):
        return {"packs": [{"id": n, "articles": a.count,
                           "first_article_id": (a.entries[0].id if isinstance(a, pwpk.Reader) else a.entries[0]["id"]) if a.count else None,
                           "article_ids": [e.id if isinstance(a, pwpk.Reader) else e["id"] for e in a.entries[:50]],
                           "expected_sha256": {str(e.id if isinstance(a, pwpk.Reader) else e["id"]): hashlib.sha256(a.extract(e.id) if isinstance(a, pwpk.Reader) else a.article_bytes(e)).hexdigest() for e in a.entries[:50]},
                           "dictionary_bytes":
                           len(getattr(a, "dictionary", getattr(a, "dict", b""))),
                           "format_version": getattr(a, "format_version", 1),
                           "codec": getattr(a, "codec", "zstd")}
                          for n, a in sorted(self.packs.items())]}


INDEX_HTML = r'''<!doctype html><meta charset="utf-8"><title>PWPK browser test</title>
<pre id="out">loading…</pre><script type="module">
// Native zstd is the zero-asset path. The vendored @bokuweb/zstd-wasm
// fallback is loaded when native decoding is unavailable.
let wasm; const dictCache = new Map();
async function wasmDecode(frame, dictionary) {
  if (!wasm) { wasm = await import('/vendor/index.web.js'); await wasm.init(); }
  const ctx = wasm.createDCtx();
  try { return dictionary?.length ? wasm.decompressUsingDict(ctx, frame, dictionary) : wasm.decompress(frame); }
  finally { wasm.freeDCtx(ctx); }
}
export async function decode(frame, dictionary) {
  let native = false;
  if (typeof DecompressionStream !== "undefined") {
    try { const result = await new Response(
      new Blob([frame]).stream().pipeThrough(new DecompressionStream("zstd"))
    ).arrayBuffer(); if (result.byteLength) { native = true; return {bytes:new Uint8Array(result), native}; } } catch (_) {}
  }
  return {bytes: await wasmDecode(frame, dictionary), native};
}
window.pwpkDecode = decode;
async function run() {
  const m = await (await fetch('/api/packs')).json();
  const p = m.packs[0]; const meta = await (await fetch(`/api/packs/${p.id}/articles/${p.first_article_id}`)).json();
  const frame = new Uint8Array(await (await fetch(meta.url)).arrayBuffer());
  try { let dict = null; if (p.dictionary_bytes) { if (!dictCache.has(p.id)) dictCache.set(p.id, fetch(`/api/packs/${p.id}/dictionary`).then(r=>r.arrayBuffer())); dict = new Uint8Array(await dictCache.get(p.id)); }
    const result = await decode(frame, dict); const body = result.bytes;
    out.textContent = JSON.stringify({native_zstd: result.native, bytes: body.length, expected: meta.raw_bytes}, null, 2);
  } catch (e) { out.textContent = JSON.stringify({native_zstd: false, error: String(e)}, null, 2); }
}
run().catch(e => { out.textContent = JSON.stringify({native_zstd: false, wasm_error: String(e), stack: e?.stack}, null, 2); });
</script>'''

BENCHMARK_HTML = r'''<!doctype html><meta charset="utf-8"><title>PWPK browser benchmark</title><pre id="out">running…</pre><script type="module">
const w=await import('/vendor/index.web.js'); await w.init(); const packs=(await (await fetch('/api/packs')).json()).packs; const rows=[];
for(const p of packs){let ids=p.article_ids; let times=[],bytes=0,ok=0,dictFetch=0,dict=null;
 if(p.dictionary_bytes){dict=new Uint8Array(await (await fetch(`/api/packs/${p.id}/dictionary`)).arrayBuffer());dictFetch=1}
 for(const id of ids){let m=await (await fetch(`/api/packs/${p.id}/articles/${id}`)).json(), fr=await fetch(m.url), f=new Uint8Array(await fr.arrayBuffer());bytes+=f.length;let t=performance.now(), d=w.createDCtx(), raw=p.dictionary_bytes?w.decompressUsingDict(d,f,dict):w.decompress(f);w.freeDCtx(d);times.push(performance.now()-t);let slice=raw.slice(m.raw_offset,m.raw_offset+m.raw_bytes), dig=await crypto.subtle.digest('SHA-256',slice), hex=[...new Uint8Array(dig)].map(x=>x.toString(16).padStart(2,'0')).join('');if(slice.length===m.raw_bytes && hex===p.expected_sha256[id])ok++}
 times.sort((a,b)=>a-b); rows.push({pack:p.id,requests:ids.length,successes:ok,total_frame_bytes:bytes,dictionary_fetches:dictFetch,median_ms:times[Math.floor(times.length/2)],p95_ms:times[Math.floor(times.length*.95)]})}
let ce={supported:false,error:null}; try {let q=packs.find(x=>x.id==='independent')||packs.find(x=>!x.dictionary_bytes),id=q.article_ids[0],m=await (await fetch(`/api/packs/${q.id}/articles/${id}`)).json(),res=await fetch(m.url+'?ce=1'),b=new Uint8Array(await res.arrayBuffer()),h=await crypto.subtle.digest('SHA-256',b),hex=[...new Uint8Array(h)].map(x=>x.toString(16).padStart(2,'0')).join('');ce={pack:q.id,supported:hex===q.expected_sha256[id],bytes:b.length,content_encoding:res.headers.get('Content-Encoding')}} catch(e){ce.error=String(e)}
let r={user_agent:navigator.userAgent,secure_context:isSecureContext,native_content_encoding:ce,rows}; await fetch('/benchmark-results',{method:'POST',body:JSON.stringify(r)});out.textContent=JSON.stringify(r,null,2);
</script>'''


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    packset: PackSet | None = None
    chunk_size = 8192

    def log_message(self, *_args):
        pass

    def send_bytes(self, data, ctype, status=200, extra=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers()
        for i in range(0, len(data), self.chunk_size): self.wfile.write(data[i:i+self.chunk_size])

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        path = u.path.strip("/").split("/")
        if u.path == "/" or u.path == "/index.html":
            return self.send_bytes(INDEX_HTML.encode(), "text/html; charset=utf-8")
        if u.path == "/benchmark":
            return self.send_bytes(BENCHMARK_HTML.encode(), "text/html; charset=utf-8")
        if u.path == "/api/packs":
            return self.send_bytes(json.dumps(self.packset.manifest()).encode(), "application/json")
        if len(path) < 4 or path[0] != "api" or path[1] != "packs":
            if path and path[0] == "vendor":
                base = Path(__file__).resolve().parent.parent / "assets" / "pwpk-ui" / "vendor"
                f = base.joinpath(*path[1:]).resolve()
                if not f.is_file() and f.suffix == "": f = f.with_suffix(".js")
                if not f.is_file() or base.resolve() not in f.parents: return self.send_bytes(b"not found", "text/plain", 404)
                ctype = "application/wasm" if f.suffix == ".wasm" else "text/javascript; charset=utf-8"
                return self.send_bytes(f.read_bytes(), ctype)
            return self.send_bytes(b"not found", "text/plain", 404)
        name = urllib.parse.unquote(path[2]); arch = self.packset.packs.get(name)
        if arch is None: return self.send_bytes(b"unknown pack", "text/plain", 404)
        dictionary = getattr(arch, "dictionary", getattr(arch, "dict", b""))
        if path[3] == "dictionary":
            return self.send_bytes(dictionary, "application/octet-stream")
        if len(path) < 5 or path[3] != "articles": return self.send_bytes(b"not found", "text/plain", 404)
        try: article_id = int(path[4])
        except ValueError: return self.send_bytes(b"bad article", "text/plain", 400)
        try:
            e = arch.lookup(article_id) if isinstance(arch, pwpk.Reader) else arch.entries[article_id]
        except (KeyError, IndexError): return self.send_bytes(b"not found", "text/plain", 404)
        if isinstance(arch, pwpk.Reader):
            start, frame_len = arch.compressed_range(article_id)
            frame = arch.data[start:start + frame_len]
        else:
            start = arch.payload_offset + e["content_offset"]
            frame = arch.content[start:start + e["comp_len"]]
        if len(path) == 5:
            title = arch.title(e) if isinstance(arch, pwpk.Reader) else e["disp"].decode()
            raw_len = e.raw_size if isinstance(arch, pwpk.Reader) else e["uncomp_len"]
            raw_offset = e.raw_offset if isinstance(arch, pwpk.Reader) else 0
            unit_raw = e.unit_raw_length if isinstance(arch, pwpk.Reader) else raw_len
            crc = e.crc32 if isinstance(arch, pwpk.Reader) else e["crc32"]
            body = json.dumps({"id": article_id, "title": title,
                               "raw_bytes": raw_len, "compressed_bytes": len(frame),
                               "raw_offset": raw_offset, "unit_raw_bytes": unit_raw,
                               "crc32": crc,
                               "dictionary_bytes": len(dictionary),
                               "url": f"/api/packs/{urllib.parse.quote(name)}/articles/{article_id}/compressed"}).encode()
            return self.send_bytes(body, "application/json")
        if path[5] != "compressed": return self.send_bytes(b"not found", "text/plain", 404)
        # HTTP Range is useful when the route is backed by a raw flash region.
        rng = self.headers.get("Range"); lo, hi = 0, len(frame) - 1
        if rng and rng.startswith("bytes="):
            total = len(frame)
            try:
                a, b = rng[6:].split("-", 1); lo = int(a); hi = int(b) if b else hi
                if lo < 0 or hi < lo or hi >= len(frame): raise ValueError
            except ValueError: return self.send_bytes(b"bad range", "text/plain", 416)
            frame = frame[lo:hi+1]
            return self.send_bytes(frame, "application/zstd", 206,
                                   {"Content-Range": f"bytes {lo}-{hi}/{total}"})
        codec = getattr(arch, "codec", "zstd")
        ctype = "application/zstd" if codec == "zstd" else "application/octet-stream"
        extra = {"X-PWPK-Compressed": codec}
        # Content-Encoding is opt-in, because dictionary frames cannot be
        # decoded by the HTTP stack without a negotiated dictionary.
        if codec == "zstd" and not dictionary and urllib.parse.parse_qs(u.query).get("ce") == ["1"] and self.headers.get("Accept-Encoding", "").find("zstd") >= 0:
            extra["Content-Encoding"] = "zstd"
        return self.send_bytes(frame, ctype, 200, extra)

    def do_POST(self):
        if self.path != "/benchmark-results": self.send_bytes(b"not found", "text/plain", 404); return
        n = int(self.headers.get("Content-Length", "0")); body = self.rfile.read(n)
        out = Path("benchmarks/browser_results.json"); out.parent.mkdir(parents=True, exist_ok=True); out.write_bytes(body)
        self.send_bytes(b"ok", "text/plain")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+", help="pack directories or .pwp files")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args(argv)
    Handler.packset = PackSet(args.paths)
    server = http.server.ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"PWPK browser server on http://{args.host}:{args.port}/ ({len(Handler.packset.packs)} packs)")
    try: server.serve_forever()
    except KeyboardInterrupt: pass


if __name__ == "__main__": main()
