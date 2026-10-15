#!/usr/bin/env python3
"""Build and run the PocketWiki device simulator.

The simulator compiles the firmware's own web server, archive reader, pack
store and catalogue reader for the host (see sim/), so a change to
firmware/main/ can be exercised in a browser or by tests in seconds, without
flashing a board. It keeps the device's limits: the C3 or S3 pack-store
capacity, the single-task HTTP server and its socket pool, the 1 KiB request
header and 512-byte URI budgets, and IDF's query-string truncation rules.

    python3 tools/pwsim.py run                    # C3, http://127.0.0.1:8080/
    python3 tools/pwsim.py run --target s3 --port 9000
    python3 tools/pwsim.py run --pack build/biology.pwp --fresh
    python3 tools/pwsim.py build                  # compile only

It does not model the radio, BLE, heap pressure, or flash timing (beyond an
optional per-read delay); confirm those on hardware.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FW = REPO / "firmware" / "main"
MINIZ = REPO / "firmware" / "third_party" / "miniz"
SIM = REPO / "sim"
BUILD = REPO / "build" / "sim"

FIRMWARE_SOURCES = [
    "web_server.c", "content_archive.c", "pack_store.c", "title_lookup.c",
    "catalog_index.c", "html_helpers.c", "style_asset.c", "catalog_asset.c",
    "manager_js_asset.c",
]

sys.path.insert(0, str(REPO / "tools"))


def _sources() -> list[Path]:
    return ([FW / name for name in FIRMWARE_SOURCES]
            + sorted((SIM / "src").glob("*.c"))
            + [MINIZ / "miniz.c"])


def _inputs() -> list[Path]:
    return (_sources() + sorted(FW.glob("*.h")) + sorted((SIM / "include").rglob("*.h"))
            + sorted((SIM / "src").glob("*.h")) + [Path(__file__)])


def have_libcurl() -> bool:
    """True when the C compiler can see libcurl's headers (the HTTP client)."""
    cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if cc is None:
        return False
    probe = subprocess.run([cc, "-E", "-x", "c", "-"], input="#include <curl/curl.h>\n",
                           capture_output=True, text=True)
    return probe.returncode == 0


def refresh_assets() -> None:
    """Regenerate the embedded stylesheet, catalogue and manager script."""
    for tool in ("embed_style.py", "embed_catalog.py", "embed_manager_js.py"):
        subprocess.run([sys.executable, str(REPO / "tools" / tool)], check=True,
                       stdout=subprocess.DEVNULL)


def build(sanitize: bool = True, force: bool = False, quiet: bool = False) -> Path:
    """Compile the simulator if any input changed; return the binary path."""
    cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if cc is None:
        raise SystemExit("pwsim: no C compiler found")
    refresh_assets()
    out = BUILD / ("pwsim-asan" if sanitize else "pwsim")
    if not force and out.exists():
        newest = max(p.stat().st_mtime for p in _inputs())
        if out.stat().st_mtime >= newest:
            return out
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [cc, "-std=gnu11", "-g", "-O1", "-pthread", "-Wall", "-Wextra", "-Werror",
           "-Wno-unused-parameter", "-Wno-sign-compare",
           "-include", str(SIM / "include" / "sdkconfig.h"),
           "-I", str(SIM / "include"), "-I", str(SIM / "src"), "-I", str(FW),
           "-I", str(MINIZ), '-DPW_PACK_DIR="packs"']
    if sanitize:
        cmd += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-fno-sanitize-recover=undefined"]
    # miniz is vendored third-party code; keep its warnings out of -Werror.
    objs = []
    for src in _sources():
        obj = BUILD / "obj" / ("asan" if sanitize else "plain") / (src.stem + ".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        extra = ["-Wno-error"] if src.parent == MINIZ else []
        subprocess.run(cmd + extra + ["-c", str(src), "-o", str(obj)], check=True)
        objs.append(str(obj))
    link = ([cc, "-pthread"] + (["-fsanitize=address,undefined"] if sanitize else [])
            + objs + ["-lcurl", "-o", str(out)])
    subprocess.run(link, check=True)
    if not quiet:
        print(f"pwsim: built {out.relative_to(REPO)}")
    return out


def build_guide(articles: Path, out: Path) -> tuple[Path, Path]:
    """Pack the built-in library and return (content.bin, index.bin)."""
    import pack_content

    pack_content.build(str(articles), str(out))
    return out / "content.bin", out / "index.bin"


def command(binary: Path, data: Path, content: Path, index: Path, *, port: int = 8080,
            target: str = "c3", uplink: bool = True, log: str = "I",
            flash_read_us: int = 0) -> list[str]:
    cmd = [str(binary), "--data", str(data), "--content", str(content),
           "--index", str(index), "--port", str(port), "--target", target,
           "--log", log]
    if not uplink:
        cmd.append("--no-uplink")
    if flash_read_us:
        cmd += ["--flash-read-us", str(flash_read_us)]
    return cmd


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="compile the simulator")
    b.add_argument("--no-sanitize", action="store_true",
                   help="build without AddressSanitizer/UBSan")
    b.add_argument("--force", action="store_true")
    r = sub.add_parser("run", help="build and start a simulated device")
    r.add_argument("--target", choices=("c3", "s3"), default="c3")
    r.add_argument("--port", type=int, default=8080)
    r.add_argument("--articles", type=Path, default=REPO / "articles" / "pocketwiki",
                   help="source of the built-in library (default: the guide)")
    r.add_argument("--data", type=Path, default=None,
                   help="device state directory (default: build/sim/<target>)")
    r.add_argument("--pack", type=Path, action="append", default=[],
                   help="pre-install a .pwp file (repeatable)")
    r.add_argument("--fresh", action="store_true", help="erase installed packs first")
    r.add_argument("--no-uplink", action="store_true",
                   help="start with the station Wi-Fi disconnected")
    r.add_argument("--no-sanitize", action="store_true")
    r.add_argument("--log", default="I", choices=list("EWIDV"))
    r.add_argument("--flash-read-us", type=int, default=0,
                   help="delay added to each built-in partition read")
    r.add_argument("--map-url", action="append", default=[],
                   help="FROM=TO URL prefix rewrite for device downloads")
    args = ap.parse_args(argv)

    if args.cmd == "build":
        build(sanitize=not args.no_sanitize, force=args.force)
        return 0

    binary = build(sanitize=not args.no_sanitize)
    data = args.data or BUILD / args.target
    packs = data / "packs"
    if args.fresh and packs.exists():
        shutil.rmtree(packs)
    packs.mkdir(parents=True, exist_ok=True)
    for pwp in args.pack:
        shutil.copyfile(pwp, packs / pwp.name)
    content, index = build_guide(args.articles, data / "guide")
    env = dict(os.environ)
    if args.map_url:
        env["PW_SIM_URL_MAP"] = ";".join(args.map_url)
    cmd = command(binary, data, content, index, port=args.port, target=args.target,
                  uplink=not args.no_uplink, log=args.log,
                  flash_read_us=args.flash_read_us)
    print(f"pwsim: {args.target.upper()} device at http://127.0.0.1:{args.port}/ "
          f"(state in {data})", flush=True)
    os.execvpe(cmd[0], cmd, env)


if __name__ == "__main__":
    raise SystemExit(main())
