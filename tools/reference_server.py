#!/usr/bin/env python3
"""Host-side reference server for PocketWiki archives.

Serves the exact generated index.bin/content.bin over HTTP with the same
routes, headers and escaping rules as the firmware, so the archive format
and the frontend can be tested without hardware:

    python tools/reference_server.py build/content/ [--port 8080]

The Handler class is importable: tests set Handler.arch / Handler.style.
"""

from __future__ import annotations

import argparse
import http.server
import json
import os
import sys
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import archive_format as af                    # noqa: E402
from sanitize import escape_html               # noqa: E402

PAGE_SIZE = 100
SEARCH_LIMIT = 20


def load(out_dir: str) -> af.Archive:
    with open(os.path.join(out_dir, "content.bin"), "rb") as fh:
        content = fh.read()
    with open(os.path.join(out_dir, "index.bin"), "rb") as fh:
        index = fh.read()
    return af.Archive(content, index)


def strict_unquote(s: str) -> str | None:
    """Strict percent-decode mirroring firmware url_decode_path."""
    out = bytearray()
    i = 0
    while i < len(s):
        if s[i] == "%":
            if i + 2 >= len(s):
                return None
            try:
                b = int(s[i + 1:i + 3], 16)
            except ValueError:
                return None
            if b < 0x20 or b == 0x7F:
                return None
            out.append(b)
            i += 3
        else:
            o = ord(s[i])
            if o < 0x20 or o == 0x7F:
                return None
            out.extend(s[i].encode("utf-8"))
            i += 1
    return out.decode("utf-8")


class Handler(http.server.BaseHTTPRequestHandler):
    arch: af.Archive | None = None
    style: bytes = b""
    server_version = "PocketWikiRef/1.0"
    # Streaming pages do not precompute Content-Length; HTTP/1.0 connection
    # close cleanly delimits those responses in this host-only preview server.
    protocol_version = "HTTP/1.0"

    # ---- helpers ----
    def send(self, code, ctype, body: bytes, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def page_open(self, title_esc: str):
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.end_headers()
        self.wfile.write(
            ('<!doctype html><html><head><meta charset="utf-8">'
             '<meta name="viewport" content="width=device-width,initial-scale=1">'
             '<link rel="stylesheet" href="/style.css?v=2">'
             f'<title>PocketWiki{title_esc}</title></head><body>'
             '<!-- THESIS: Setup and reading stay obvious, never dashboard-like. OWN-WORLD: warm paper, ink, forest green, quiet cards, square P. STORY: connect, install, read. FIRST VIEWPORT: compact identity and search lead a single-column task surface. FORM: setup-first. FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance. -->'
             '<header>'
             '<div class="header-inner">'
             '<a class="brand" href="/" aria-label="PocketWiki home">'
             '<span class="brand-mark" aria-hidden="true">P</span>'
             '<span>PocketWiki</span></a>'
             '<nav aria-label="Main navigation"><a href="/manage">Manage library</a></nav>'
             '<form class="search" action="/search" method="get">'
             '<label class="sr-only" for="site-search">Search article titles</label>'
             '<input id="site-search" type="search" name="q" '
             'placeholder="Search the library" autocomplete="off" maxlength="128">'
             '<button type="submit">Search</button></form></div></header><main>'
             ).encode("utf-8"))

    def page_close(self):
        self.wfile.write(
            (f'</main><footer>PocketWiki offline &middot; Reading stays local &middot; {self.arch.count} articles'
             '</footer></body></html>').encode("utf-8"))

    def error_page(self, code, title, detail):
        body = (f'<!doctype html><html><head><meta charset="utf-8">'
                f'<meta name="viewport" content="width=device-width,initial-scale=1">'
                f'<link rel="stylesheet" href="/style.css?v=2">'
                f'<title>PocketWiki &middot; error</title></head><body>'
                f'<header><div class="header-inner"><a class="brand" href="/">'
                f'<span class="brand-mark" aria-hidden="true">P</span>'
                f'<span>PocketWiki</span></a></div></header><main>'
                f'<div class="err"><h1>{escape_html(title)}</h1>'
                f'<p>{escape_html(detail)}</p>'
                f'<p><a href="/">Back to the homepage</a></p></div></main>'
                f'</body></html>').encode("utf-8")
        self.send(code, "text/html; charset=utf-8", body)

    def list_item(self, eid: int, disp: str, bold: bool = False):
        t = f'<a href="/a/{eid}">' + ("<strong>" if bold else "") + \
            escape_html(disp) + ("</strong>" if bold else "") + "</a>"
        self.wfile.write(t.encode("utf-8"))

    def send_article(self, e: dict, download: bool = False):
        arch = self.arch
        etag = "crc-%08x" % e["crc32"]
        inm = self.headers.get("If-None-Match")
        if inm and inm == etag and not download:
            self.send_response(304)
            self.send_header("ETag", etag)
            self.end_headers()
            return
        # Mirrors the firmware: the stored body is a zstd frame decoded with
        # the pack's shared dictionary; both view and download serve identity
        # HTML (no Content-Encoding).
        body = arch.article_bytes(e)
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        if not download:
            self.send_header("ETag", etag)
        self.send_header("Cache-Control", "no-store" if download else "public, max-age=86400")
        if download:
            self.send_header("Content-Disposition", f'attachment; filename="pocketwiki-{e["id"]}.html"')
        self.end_headers()
        self.wfile.write(body)

    # ---- routes ----
    def do_GET(self):
        arch = self.arch
        parsed = urllib.parse.urlsplit(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)

        if path == "/":
            self.home(qs)
        elif path == "/search":
            self.search(qs)
        elif path.startswith("/a/"):
            self.by_id(path[3:])
        elif path.startswith("/download/"):
            self.by_id(path[10:], download=True)
        elif path.startswith("/title/"):
            self.by_title(path[7:])
        elif path == "/style.css":
            self.send(200, "text/css; charset=utf-8", self.style,
                      {"Cache-Control": "public, max-age=86400"})
        elif path == "/manage":
            self.packs()
        elif path == "/packs":
            self.send_response(301)
            self.send_header("Location", "/manage")
            self.end_headers()
        elif path == "/health":
            self.send(200, "text/plain; charset=utf-8", b"ok\n")
        elif path == "/api/stats":
            stats = json.dumps({
                "articles": arch.count,
                "format_version": af.FORMAT_VERSION,
                "archive_ok": True,
                "content_bytes": arch.payload_size,
                "index_bytes": len(arch.index),
                "uptime_s": 0,
            }).encode("utf-8")
            self.send(200, "application/json; charset=utf-8", stats)
        else:
            self.error_page(404, "Not found", "There is no such page on this device.")

    def home(self, qs):
        arch = self.arch
        try:
            page = int(qs.get("p", ["0"])[0])
        except ValueError:
            return self.error_page(400, "Bad request", "Invalid page parameter.")
        start = page * PAGE_SIZE
        if start >= arch.count:
            return self.error_page(404, "Page not found", "That browse page does not exist.")
        self.page_open(" &middot; article list")
        end = min(start + PAGE_SIZE, arch.count)
        self.wfile.write(
            (f'<h1>Articles</h1><p class="muted">{arch.count} articles, '
             f'showing {start + 1}&ndash;{end}</p><div class="list">').encode("utf-8"))
        for e in arch.entries[start:end]:
            self.list_item(e["id"], e["disp"].decode("utf-8"))
        self.wfile.write(b"</div>")
        pages = (arch.count + PAGE_SIZE - 1) // PAGE_SIZE
        nav = f'<p class="muted">Page {page + 1} of {pages}'
        if page > 0:
            nav += f' &middot; <a href="/?p={page - 1}">Newer</a>'
        if end < arch.count:
            nav += f' &middot; <a href="/?p={page + 1}">Older</a>'
        self.wfile.write((nav + "</p>").encode("utf-8"))
        self.page_close()

    def search(self, qs):
        arch = self.arch
        q = qs.get("q", [""])[0]
        if len(q) > 128:
            return self.error_page(400, "Bad query", "Search query too long.")
        self.page_open(" &middot; search")
        self.wfile.write(
            (f'<h1>Search</h1><p class="muted">Results for &ldquo;'
             f'{escape_html(q)}&rdquo;</p>').encode("utf-8"))
        res = arch.search(q, SEARCH_LIMIT)
        if not res:
            self.wfile.write(b"<p>No articles match.</p>")
        else:
            self.wfile.write(b'<div class="list">')
            for i, e in enumerate(res):
                self.list_item(e["id"], e["disp"].decode("utf-8"),
                               bold=(i == 0 and arch.find_exact(q) is not None))
            self.wfile.write(b"</div>")
            if len(res) >= SEARCH_LIMIT:
                self.wfile.write(b'<p class="muted">First 20 matches shown.</p>')
        self.page_close()

    def packs(self):
        self.page_open(" &middot; manage")
        self.wfile.write(
            (f'<h1>Manage library</h1><p class="muted">The included library is ready. Add more packs when you want them.</p>'
             '<div class="list pack-list"><p><strong>Built-in Library</strong> &middot; '
             f'{self.arch.count} articles &middot; included and always available</p></div>'
             '<section class="manager-section"><h2>Install from a file or URL</h2>'
             '<form class="upload"><label>Pack file on this device'
             '<input type="file" accept=".pwp"></label>'
             '<button type="button">Install pack file</button></form>'
             '<form class="upload"><label>Secure pack URL'
             '<input type="url" placeholder="https://example.com/library.pwp"></label>'
             '<button type="button">Download and install</button></form></section>'
             '<section class="manager-section"><h2>Connect PocketWiki to Wi-Fi</h2>'
             '<p class="muted">Optional: connect to a 2.4 GHz network to refresh the catalogue and download packs. The PocketWiki access point stays available for reading.</p>'
             '<form class="upload"><label>Wi-Fi network name<input></label>'
             '<label>Wi-Fi password<input type="password"></label>'
             '<button type="button">Save and connect</button></form></section>'
             '<p><a href="/">&larr; Back to your libraries</a></p>').encode("utf-8"))
        self.page_close()

    def by_id(self, s: str, download: bool = False):
        arch = self.arch
        if not s.isdigit():
            return self.error_page(400, "Bad article id",
                                   "Article ids are plain numbers.")
        eid = int(s)
        if eid >= arch.count:
            return self.error_page(404, "Article not found",
                                   "No article has this identifier.")
        self.send_article(arch.entries[eid], download=download)

    def by_title(self, s: str):
        arch = self.arch
        dec = strict_unquote(s)
        if dec is None:
            return self.error_page(400, "Bad title", "The title could not be decoded.")
        e = arch.find_exact(dec)
        if e is None:
            return self.error_page(404, "Article not found",
                                   "No article has this title. Use the search page.")
        self.send_article(e)


def serve(out_dir: str, port: int):
    Handler.arch = load(out_dir)
    with open(os.path.join(os.path.dirname(__file__), "..", "assets", "style.css"),
              "rb") as fh:
        Handler.style = fh.read()
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"reference server: {Handler.arch.count} articles on http://127.0.0.1:{port}/")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="PocketWiki reference server")
    ap.add_argument("out_dir", help="directory with content.bin + index.bin")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args(argv)
    serve(args.out_dir, args.port)
    return 0


if __name__ == "__main__":
    sys.exit(main())
