"""End-to-end tests of the reference server against the real sample archive."""

import json
import os
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

import pack_content                  # noqa: E402
import reference_server              # noqa: E402


class Server:
    def __init__(self, arch):
        self.arch = arch
        reference_server.Handler.arch = arch
        with open(os.path.join(REPO, "assets", "style.css"), "rb") as fh:
            reference_server.Handler.style = fh.read()
        self.httpd = None
        self.port = None
        self._thread = None

    def __enter__(self):
        self.httpd = type(self.httpd or object)  # placeholder, replaced below
        import http.server
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0),
                                                     reference_server.Handler)
        self.port = self.httpd.server_address[1]
        self._thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self.httpd.shutdown()
        self._thread.join(timeout=5)

    def get(self, path, headers=None):
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}",
                                     headers=headers or {})
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                return resp.status, dict(resp.headers), resp.read()
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()


@pytest.fixture(scope="module")
def server(tmp_path_factory):
    out = tmp_path_factory.mktemp("ref")
    pack_content.build(os.path.join(REPO, "tests", "fixtures", "articles", "sample"), str(out))
    arch = reference_server.load(str(out))
    with Server(arch) as s:
        yield s


def test_homepage(server):
    status, _, body = server.get("/")
    assert status == 200
    assert b"PocketWiki" in body and b'/style.css' in body
    assert b'/a/' in body           # browse list present
    assert b'aria-label="Main navigation"' in body
    assert b'placeholder="Search the library"' in body


def test_article_served_as_identity_html(server):
    e = server.arch.find_exact("arduino")
    status, headers, body = server.get(f"/a/{e['id']}")
    assert status == 200
    # The device decodes the stored zstd frame: identity HTML, no encoding.
    assert "Content-Encoding" not in headers
    assert headers["Content-Type"].startswith("text/html")
    assert int(headers["Content-Length"]) == e["uncomp_len"]
    assert headers["ETag"] == "crc-%08x" % e["crc32"]
    html = body.decode("utf-8")
    assert "<h1>Arduino</h1>" in html


def test_etag_304(server):
    e = server.arch.find_exact("arduino")
    etag = "crc-%08x" % e["crc32"]
    status, _, body = server.get(f"/a/{e['id']}", headers={"If-None-Match": etag})
    assert status == 304 and body == b""


def test_article_download_is_plain_html_attachment_and_never_304(server):
    e = server.arch.find_exact("arduino")
    etag = "crc-%08x" % e["crc32"]
    status, headers, body = server.get(
        f"/download/{e['id']}", headers={"If-None-Match": etag})
    assert status == 200
    assert headers["Content-Disposition"].startswith("attachment;")
    assert headers["Cache-Control"] == "no-store"
    # The attachment is plain HTML with no Content-Encoding to undo: a
    # browser's download manager can open the saved file as-is.
    assert "Content-Encoding" not in headers
    assert body.startswith(b"<!doctype html>")
    assert b"Arduino" in body


def test_article_by_id_404(server):
    status, _, body = server.get("/a/99999")
    assert status == 404 and b"Article not found" in body


def test_article_by_id_bad(server):
    status, _, _ = server.get("/a/12abc")
    assert status == 400


def test_article_by_title(server):
    path = "/title/" + urllib.parse.quote("Arduino")
    status, headers, body = server.get(path)
    assert status == 200 and "Content-Encoding" not in headers
    assert b"Arduino" in body


def test_article_by_title_404(server):
    status, _, _ = server.get("/title/" + urllib.parse.quote("No Such Article"))
    assert status == 404


def test_article_by_title_encoding_error(server):
    status, _, _ = server.get("/title/%zz")
    assert status == 400


def test_search(server):
    status, _, body = server.get("/search?q=arduino")
    assert status == 200
    e = server.arch.find_exact("arduino")
    assert f'/a/{e["id"]}'.encode() in body
    assert b"Arduino" in body


def test_search_no_results(server):
    status, _, body = server.get("/search?q=zzzzz")
    assert status == 200 and b"No articles match" in body


def test_search_echo_escaped(server):
    status, _, body = server.get("/search?q=" + urllib.parse.quote("<script>"))
    assert status == 200
    assert b"<script>" not in body
    assert b"&lt;script&gt;" in body


def test_health(server):
    status, _, body = server.get("/health")
    assert status == 200 and body == b"ok\n"


def test_stats(server):
    status, _, body = server.get("/api/stats")
    assert status == 200
    data = json.loads(body)
    assert data["articles"] == server.arch.count
    assert data["archive_ok"] is True


def test_style_matches_canonical(server):
    status, _, body = server.get("/style.css")
    assert status == 200
    with open(os.path.join(REPO, "assets", "style.css"), "rb") as fh:
        assert body == fh.read()


def test_packs_page(server):
    status, _, body = server.get("/manage")
    assert status == 200
    assert b"Manage library" in body
    assert b"Connect PocketWiki to Wi-Fi" in body
    assert b"Install pack file" in body


def test_legacy_packs_route_redirects_to_manage(server):
    status, headers, _ = server.get("/packs")
    assert status == 200  # urllib follows the compatibility redirect
    assert headers.get("Content-Type", "").startswith("text/html")


def test_unknown_route(server):
    status, _, _ = server.get("/nope")
    assert status == 404
