import json
import threading
import urllib.request

import pwpk_browser_server as srv
import pwpk


def test_browser_server_manifest_and_ranges(tmp_path, make_archive):
    arch = make_archive({"a.md": "# Alpha\n\nhello alpha", "b.md": "# Beta\n\nhello beta"})
    (tmp_path / "content.bin").write_bytes(arch.content)
    (tmp_path / "index.bin").write_bytes(arch.index)
    srv.Handler.packset = srv.PackSet([tmp_path])
    httpd = __import__("http.server").server.ThreadingHTTPServer(("127.0.0.1", 0), srv.Handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True); thread.start()
    base = "http://127.0.0.1:%d" % httpd.server_address[1]
    try:
        manifest = json.load(urllib.request.urlopen(base + "/api/packs"))
        assert manifest["packs"][0]["articles"] == 2
        meta = json.load(urllib.request.urlopen(base + "/api/packs/%s/articles/0" % tmp_path.name))
        frame = urllib.request.urlopen(base + meta["url"]).read()
        req = urllib.request.Request(base + meta["url"], headers={"Range": "bytes=0-3"})
        part = urllib.request.urlopen(req)
        assert part.status == 206 and part.read() == frame[:4]
        assert frame[:4] == b"(\xb5/\xfd"
    finally:
        httpd.shutdown(); thread.join(timeout=2); httpd.server_close()


def test_browser_server_reads_experimental_pwpk(tmp_path):
    data = pwpk.build_pack([pwpk.Article(42, "Answer", b"payload")])
    pack = tmp_path / "answer.pwpk"; pack.write_bytes(data)
    srv.Handler.packset = srv.PackSet([pack])
    httpd = __import__("http.server").server.ThreadingHTTPServer(("127.0.0.1", 0), srv.Handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True); thread.start()
    base = "http://127.0.0.1:%d" % httpd.server_address[1]
    try:
        meta = json.load(urllib.request.urlopen(base + "/api/packs/answer/articles/42"))
        assert meta["title"] == "Answer"
        frame = urllib.request.urlopen(base + meta["url"]).read()
        assert frame[:4] == b"(\xb5/\xfd"
    finally:
        httpd.shutdown(); thread.join(timeout=2); httpd.server_close()
