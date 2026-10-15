"""The firmware's web server, archive reader and pack store, run on the host.

Each test boots tools/pwsim.py's simulator - the real firmware modules built
with AddressSanitizer and UBSan - against a fresh pack store, then drives it
over HTTP the way a phone does. The simulator runs in strict mode: a handler
that returns without answering or leaves a chunked response open aborts it,
and every test fails if the device crashed or logged a protocol error.
"""

from __future__ import annotations

import functools
import hashlib
import http.client
import http.server
import json
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import archive_format as af  # noqa: E402
import pack_content  # noqa: E402

pwsim = pytest.importorskip("pwsim")


@pytest.fixture(scope="session")
def sim_binary():
    if not pwsim.have_libcurl():
        pytest.skip("libcurl development files are not installed")
    try:
        return pwsim.build(quiet=True)
    except subprocess.CalledProcessError as exc:  # pragma: no cover - toolchain issue
        pytest.fail(f"simulator failed to build: {exc}")


@pytest.fixture(scope="session")
def guide(tmp_path_factory):
    out = tmp_path_factory.mktemp("guide")
    content, index = pwsim.build_guide(REPO / "articles" / "pocketwiki", out)
    return content, index


def _pack_file(src: Path, out: Path) -> bytes:
    build_dir = out.with_suffix("")
    pack_content.build(str(src), str(build_dir))
    content = (build_dir / "content.bin").read_bytes()
    index = (build_dir / "index.bin").read_bytes()
    data = af.pack_pack_file(content, index)
    out.write_bytes(data)
    return data


@pytest.fixture(scope="session")
def sample_pwp(tmp_path_factory) -> bytes:
    return _pack_file(REPO / "tests" / "fixtures" / "articles" / "sample",
                      tmp_path_factory.mktemp("pwp") / "sample.pwp")


def _articles(tmp: Path, titles: list[str]) -> Path:
    tmp.mkdir(parents=True, exist_ok=True)
    for i, title in enumerate(titles):
        (tmp / f"{i:03d}.md").write_text(f"# {title}\n\n{title} body text, version marker {tmp.name}.\n")
    return tmp


class Device:
    def __init__(self, proc: subprocess.Popen, port: int, data: Path, log: Path):
        self.proc = proc
        self.port = port
        self.data = data
        self.log = log

    def request(self, method: str, path: str, body: bytes | None = None,
                headers: dict | None = None, timeout: float = 30):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        try:
            conn.request(method, path, body=body, headers=headers or {})
            resp = conn.getresponse()
            return resp.status, dict(resp.getheaders()), resp.read()
        finally:
            conn.close()

    def get(self, path: str, **kw):
        return self.request("GET", path, **kw)

    def post(self, path: str, body: bytes = b"", **kw):
        return self.request("POST", path, body=body, **kw)

    def upload(self, name: str, data: bytes):
        status, _, body = self.post(f"/api/packs/upload?name={name}", data,
                                    headers={"Content-Type": "application/octet-stream"})
        return status, json.loads(body)

    def packs(self) -> dict:
        status, _, body = self.get("/api/packs")
        assert status == 200
        return json.loads(body)

    def log_text(self) -> str:
        return self.log.read_text(errors="replace")


@pytest.fixture
def device_factory(sim_binary, guide, tmp_path):
    started: list[Device] = []

    def start(target: str = "c3", uplink: bool = True, env: dict | None = None,
              articles: tuple[Path, Path] | None = None) -> Device:
        data = tmp_path / f"device-{len(started)}"
        (data / "packs").mkdir(parents=True)
        log = data / "device.log"
        content, index = articles or guide
        cmd = pwsim.command(sim_binary, data, content, index, port=0, target=target,
                            uplink=uplink, log="I")
        proc_env = dict(os.environ, PW_SIM_STRICT="1",
                        ASAN_OPTIONS="detect_leaks=0:abort_on_error=1")
        proc_env.update(env or {})
        out = open(log, "w")
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=out, env=proc_env, text=True)
        line = proc.stdout.readline()
        if not line.startswith("PWSIM READY"):
            proc.kill()
            pytest.fail(f"simulator did not start: {line!r}\n{log.read_text()}")
        dev = Device(proc, int(line.split("port=")[1]), data, log)
        started.append(dev)
        return dev

    yield start

    for dev in started:
        alive = dev.proc.poll() is None
        dev.proc.kill()
        dev.proc.wait()
        text = dev.log_text()
        assert alive, f"simulated device exited ({dev.proc.returncode}):\n{text[-4000:]}"
        assert "PWSIM-PROTOCOL-ERROR" not in text, text[-4000:]
        assert "AddressSanitizer" not in text and "runtime error:" not in text, text[-4000:]


@pytest.fixture
def device(device_factory):
    return device_factory()


# ---- reading -------------------------------------------------------------


def test_home_health_stats_and_style(device):
    status, headers, body = device.get("/")
    assert status == 200 and b"Your libraries" in body and b"PocketWiki Guide" in body
    assert headers.get("Transfer-Encoding") == "chunked"
    assert device.get("/health")[2] == b"ok\n"
    stats = json.loads(device.get("/api/stats")[2])
    assert stats["archive_ok"] is True and stats["format_version"] == 3
    assert stats["pack_bytes_total"] == 2347008
    status, headers, css = device.get("/style.css")
    assert status == 200 and headers["Content-Type"].startswith("text/css") and len(css) > 1000


def test_every_guide_article_decodes_like_the_packer(device, guide):
    content, index = guide
    arch = af.Archive(content.read_bytes(), index.read_bytes())
    for e in arch.entries:
        status, headers, body = device.get(f"/a/{e['id']}?pack=pocketwiki")
        assert status == 200, e
        assert body == arch.article_bytes(e)
        etag = headers["ETag"]
        assert device.get(f"/a/{e['id']}?pack=pocketwiki",
                          headers={"If-None-Match": etag})[0] == 304


def test_bad_ids_and_missing_libraries(device):
    assert device.get("/a/99999?pack=pocketwiki")[0] == 404
    assert device.get("/a/12x")[0] == 400
    assert device.get("/a/99999999999")[0] == 400
    assert device.get("/a/1?pack=nope")[0] == 404
    assert device.get("/a/1?pack=../../etc")[0] == 404
    assert device.get("/no/such/page")[0] == 404
    assert device.post("/no/such/page")[0] in (404, 405)


def test_title_route_and_search(device):
    status, _, body = device.get("/title/installing%20PACKS?pack=pocketwiki")
    assert status == 200 and b"Installing packs" in body
    assert device.get("/title/nothing%20here?pack=pocketwiki")[0] == 404
    status, _, body = device.get("/search?q=Install")
    assert status == 200 and b"Installing packs" in body
    status, _, body = device.get("/search?q=" + "x" * 300)
    assert status == 400


def test_search_matches_word_starts_after_prefixes(device, tmp_path):
    status, _, body = device.get("/search?q=wi")
    assert status == 200 and b"Connecting to Wi-Fi" in body
    assert b"Bluetooth" not in body
    status, _, body = device.get("/search?q=fi")           # after a hyphen
    assert b"Connecting to Wi-Fi" in body and b"First setup" in body
    assert body.index(b"First setup") < body.index(b"Connecting to Wi-Fi")
    assert b"Bluetooth" not in device.get("/search?q=ooth")[2]

    titles = ["War", "Warsaw", "World War II", "Cold war", "Award"]
    titles += [f"Battle {i:02d} of the war" for i in range(30)]
    pack = _pack_file(_articles(tmp_path / "wars", titles), tmp_path / "wars.pwp")
    assert device.upload("wars", pack)[0] == 200
    status, _, body = device.get("/search?q=war")
    assert status == 200
    assert body.index(b"<strong>War</strong>") < body.index(b"Warsaw") < body.index(b"Battle 00")
    assert b"Award" not in body
    assert body.count(b'<a href="/a/') == 20 and b"First 20 matches" in body
    status, _, body = device.get("/search?q=world%20war")
    assert b"World War II" in body and b"Battle" not in body


def test_browse_pages_and_partial_fragments(device):
    status, _, body = device.get("/browse?pack=pocketwiki")
    assert status == 200 and body.count(b'<a href="/a/') >= 17
    status, _, body = device.get("/browse?pack=pocketwiki&offset=10000&partial=1")
    assert status == 200 and body == b""


def test_links_without_a_pack_resolve_from_the_page_they_were_on(device, sample_pwp):
    """Packed articles link to /a/<id> and /download/<id> without a pack name.

    Another visitor opening the home page switches the device's active
    library, so such a link must not depend on it: the device redirects to the
    referring page's library, or the built-in one.
    """
    assert device.upload("sample", sample_pwp)[0] == 200
    referer = f"http://127.0.0.1:{device.port}/a/0?pack=sample#top"
    device.get("/")                                  # another reader resets the library
    status, headers, _ = device.get("/a/1", headers={"Referer": referer})
    assert status == 302 and headers["Location"] == "/a/1?pack=sample"
    status, headers, _ = device.get("/download/1", headers={"Referer": referer})
    assert status == 302 and headers["Location"] == "/download/1?pack=sample"
    status, headers, _ = device.get("/a/1")
    assert status == 302 and headers["Location"] == "/a/1?pack=pocketwiki"
    status, headers, _ = device.get("/a/1", headers={"Referer": "http://x/a/0?pack=../x"})
    assert headers["Location"] == "/a/1?pack=pocketwiki"
    status, headers, _ = device.get("/title/Batteries", headers={"Referer": referer})
    assert headers["Location"] == "/title/Batteries?pack=sample"
    status, _, body = device.get(headers["Location"])
    assert status == 200 and b"Batteries" in body


# ---- installing ----------------------------------------------------------


def test_upload_read_search_and_remove_a_pack(device, sample_pwp):
    status, result = device.upload("sample", sample_pwp)
    assert status == 200 and result["ok"] is True and result["articles"] > 0
    listed = {p["name"]: p for p in device.packs()["packs"]}
    assert listed["sample"]["articles"] == result["articles"]
    arch = af.Archive(*af.unpack_pack_file(sample_pwp))
    e = arch.entries[0]
    status, _, body = device.get(f"/a/{e['id']}?pack=sample")
    assert status == 200 and body == arch.article_bytes(e)
    first_word = e["disp"].decode().split()[0]
    assert e["disp"] in device.get("/search?q=" + urllib.parse.quote(first_word))[2]
    status, _, body = device.post("/api/packs/action?action=delete&name=sample")
    assert status == 200 and json.loads(body)["ok"] is True
    assert "sample" not in {p["name"] for p in device.packs()["packs"]}
    assert device.get(f"/a/{e['id']}?pack=sample")[0] == 404


def test_rejected_upload_keeps_previous_pack_and_explains(device, sample_pwp):
    assert device.upload("sample", sample_pwp)[0] == 200
    corrupt = bytearray(sample_pwp)
    corrupt[-10] ^= 0xFF
    status, result = device.upload("sample", bytes(corrupt))
    assert status == 422 and result["ok"] is False
    assert {p["name"] for p in device.packs()["packs"]} == {"pocketwiki", "sample"}
    assert device.get("/a/0?pack=sample")[0] == 200


def test_reject_reason_is_not_carried_over(device, sample_pwp):
    old = bytearray(sample_pwp)
    old[4] = 2                                 # an older wrapper/content version
    status, result = device.upload("old", bytes(old))
    assert status == 422
    garbage = b"\0" * 64
    status, result = device.upload("junk", garbage)
    assert status == 422
    assert "older" not in result["error"], result


def test_upload_survives_a_slow_sender(device, sample_pwp):
    """A phone that pauses longer than the 3 s receive timeout mid-upload."""
    half = len(sample_pwp) // 2
    with socket.create_connection(("127.0.0.1", device.port), timeout=30) as s:
        s.sendall((f"POST /api/packs/upload?name=slow HTTP/1.1\r\nHost: x\r\n"
                   f"Content-Length: {len(sample_pwp)}\r\n\r\n").encode() + sample_pwp[:half])
        time.sleep(4)
        s.sendall(sample_pwp[half:])
        reply = s.recv(4096)
    assert b" 200 " in reply.split(b"\r\n")[0], reply
    assert "slow" in {p["name"] for p in device.packs()["packs"]}


def _upload_status_line(device, length):
    # Send only the headers: the device must refuse before reading the body.
    with socket.create_connection(("127.0.0.1", device.port), timeout=10) as s:
        s.sendall((f"POST /api/packs/upload?name=huge HTTP/1.1\r\nHost: x\r\n"
                   f"Content-Length: {length}\r\n\r\n").encode())
        return s.recv(4096).split(b"\r\n")[0]


def test_upload_larger_than_free_space_is_refused(device):
    # The empty C3 store has more room than the 2 MiB upload cap, so fill it
    # first to bring the free space under that cap.
    with open(device.data / "packs" / "filler.dat", "wb") as f:
        f.truncate(512 * 1024)
    total = device.packs()["bytes_installable"]
    assert total + 1 <= 2048 * 1024
    assert b" 507 " in _upload_status_line(device, total + 1)


def test_upload_larger_than_the_cap_is_refused(device):
    assert b" 413 " in _upload_status_line(device, 2048 * 1024 + 1)


def test_guide_update_can_be_replaced_and_removed(device, tmp_path):
    """Installing the built-in library's own name updates it, repeatedly."""
    v1 = _pack_file(_articles(tmp_path / "v1", ["Alpha one", "Beta one"]), tmp_path / "v1.pwp")
    v2 = _pack_file(_articles(tmp_path / "v2", ["Gamma two", "Delta two", "Epsilon two"]),
                    tmp_path / "v2.pwp")
    assert device.upload("pocketwiki", v1)[1]["articles"] == 2
    assert b"Alpha one" in device.get("/a/0?pack=pocketwiki")[2]    # holds the file open
    assert device.upload("pocketwiki", v2)[1]["articles"] == 3
    builtin = device.packs()["packs"][0]
    assert builtin["updated"] is True and builtin["articles"] == 3
    status, _, body = device.get("/a/0?pack=pocketwiki")
    assert status == 200 and b"Delta two" in body
    assert device.get("/")[0] == 200
    status, _, _ = device.post("/api/packs/action?action=delete&name=pocketwiki")
    assert status == 200
    builtin = device.packs()["packs"][0]
    assert builtin["updated"] is False and builtin["articles"] == 17
    assert b"About PocketWiki" in device.get("/a/0?pack=pocketwiki")[2]


def test_storage_capacity_follows_the_target(device_factory):
    c3 = device_factory("c3").packs()
    s3 = device_factory("s3").packs()
    assert c3["bytes_total"] == 2347008
    assert s3["bytes_total"] == 14823424
    assert s3["bytes_installable"] > 6 * c3["bytes_installable"]


def test_guide_visibility_toggle(device):
    status, headers, _ = device.post("/packs/guide-toggle")
    assert status == 303 and headers["Location"] == "/manage"
    assert b"PocketWiki Guide" not in device.get("/")[2].split(b"<main>")[1]
    assert device.packs()["packs"][0]["hidden"] is True
    device.post("/packs/guide-toggle")
    assert device.packs()["packs"][0]["hidden"] is False


# ---- the device's own downloads -----------------------------------------


@pytest.fixture
def file_server(tmp_path):
    root = tmp_path / "www"
    root.mkdir()
    handler = functools.partial(_QuietHandler, directory=str(root))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    yield root, f"http://127.0.0.1:{srv.server_address[1]}"
    srv.shutdown()


class _QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def _install(device: Device, url: str, name: str | None = None) -> list[str]:
    q = "url=" + urllib.parse.quote(url, safe="")
    if name:
        q += "&name=" + name
    status, _, body = device.post("/api/packs/install?" + q, timeout=60)
    text = body.decode()
    if status != 200:
        return [f"HTTP {status}: {text}"]
    return [line for line in text.splitlines() if line]


def test_install_from_url(device_factory, file_server, sample_pwp):
    root, base = file_server
    (root / "sample.pwp").write_bytes(sample_pwp)
    dev = device_factory(env={"PW_SIM_URL_MAP": f"https://packs.test={base}"})
    lines = _install(dev, "https://packs.test/sample.pwp")
    assert lines[0] == "P:0:0" and lines[-1].startswith("OK:"), lines
    assert "sample" in {p["name"] for p in dev.packs()["packs"]}


def test_install_restarts_after_a_dropped_download(device_factory, file_server, sample_pwp):
    root, base = file_server
    (root / "sample.pwp").write_bytes(sample_pwp)
    dev = device_factory(env={"PW_SIM_URL_MAP": f"https://packs.test={base}",
                              "PW_SIM_HTTP_DROP_AT": str(len(sample_pwp) // 2)})
    lines = _install(dev, "https://packs.test/sample.pwp", name="dropped")
    assert lines[-1].startswith("OK:"), lines


def test_install_accepts_a_long_pack_url(device_factory, file_server, sample_pwp):
    root, base = file_server
    long_name = "a-rather-long-collection-name-for-a-school-library-" * 3 + "biology.pwp"
    (root / "shelves" / "2026").mkdir(parents=True)
    (root / "shelves" / "2026" / long_name).write_bytes(sample_pwp)
    dev = device_factory(env={"PW_SIM_URL_MAP": f"https://packs.test={base}"})
    lines = _install(dev, f"https://packs.test/shelves/2026/{long_name}", name="long")
    assert lines[-1].startswith("OK:"), lines


def test_install_needs_the_uplink(device_factory):
    dev = device_factory(uplink=False)
    lines = _install(dev, "https://packs.test/sample.pwp")
    assert lines[0].startswith("HTTP 503"), lines


def test_catalogue_sync_and_verified_install(device_factory, file_server, sample_pwp):
    root, base = file_server
    (root / "biology.pwp").write_bytes(sample_pwp)
    catalog = {
        "schema": 1, "catalog_version": 999, "generated_at": "2099-01-01T00:00:00Z",
        "packs": [{
            "id": "biology", "name": "Biology test shelf", "version": 1,
            "url": "https://packs.educated.space/biology.pwp",
            "bytes": len(sample_pwp), "articles": 2,
            "sha256": hashlib.sha256(sample_pwp).hexdigest(),
        }],
    }
    (root / "index.json").write_text(json.dumps(catalog))
    dev = device_factory(env={"PW_SIM_URL_MAP": f"https://packs.educated.space={base}"})
    status, _, body = dev.post("/api/packs/catalog/sync", timeout=60)
    assert status == 200 and json.loads(body)["catalog_version"] == 999
    assert json.loads(dev.get("/api/packs/catalog")[2])["catalog_version"] == 999
    lines = _install(dev, "https://packs.educated.space/biology.pwp", name="biology")
    assert lines[-1].startswith("OK:"), lines
    titles = {p["name"]: p["title"] for p in dev.packs()["packs"]}
    assert titles["biology"] == "Biology test shelf"

    # A body that does not match the catalogue's digest is refused.
    (root / "biology.pwp").write_bytes(sample_pwp[:-1] + bytes([sample_pwp[-1] ^ 1]))
    lines = _install(dev, "https://packs.educated.space/biology.pwp", name="biology2")
    assert lines[-1].startswith("ERR:") and "checksum" in lines[-1], lines


# ---- transport -----------------------------------------------------------


def test_manage_page_and_wifi_routes(device):
    status, _, body = device.get("/manage")
    assert status == 200 and b"Add a library pack" in body and b"</script>" in body
    nets = json.loads(device.get("/api/wifi/scan")[2])["networks"]
    assert nets and all("ssid" in n for n in nets)
    status, _, body = device.post("/api/wifi/config",
                                  json.dumps({"ssid": "SchoolNet", "password": "pw"}).encode(),
                                  headers={"Content-Type": "application/json"})
    assert status == 200 and json.loads(body)["ok"] is True
    assert json.loads(device.get("/api/wifi/status")[2])["ssid"] == "SchoolNet"
    assert device.post("/api/wifi/config", b"{}")[0] == 400


def test_idle_connections_do_not_lock_out_readers(device):
    idle = [socket.create_connection(("127.0.0.1", device.port)) for _ in range(12)]
    try:
        assert device.get("/health", timeout=10)[0] == 200
    finally:
        for s in idle:
            s.close()


def test_oversized_request_headers_are_refused(device):
    status, _, _ = device.get("/health", headers={"X-Big": "y" * 1500})
    assert status == 431
