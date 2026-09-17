#!/usr/bin/env python3
"""Flash firmware + content partitions to an ESP32-C3 or ESP32-S3 in one shot.

Usage:
    pio run -e esp32-c3                              # 4 MB board
    python tools/flash_all.py -p /dev/cu.usbmodem1101 \
        --fw-dir firmware/dist/esp32-c3

    pio run -e esp32-s3                              # 16 MB board
    python tools/flash_all.py -p /dev/cu.usbmodem1234561 --chip esp32s3 \
        --partitions firmware/dist/esp32-s3/partitions.csv \
        --fw-dir firmware/dist/esp32-s3

Reads the partition CSV for offsets, verifies every image fits its partition,
then calls esptool (from the IDF Python environment).

Args:
    -p/--port PORT       serial port (default: first /dev/cu.usbmodem*)
    -b/--baud BAUD       esptool baud (default 921600)
    -c/--chip CHIP       esptool chip id (default esp32c3)
    -P/--partitions CSV  partition layout describing the flash (default
                         firmware/partitions.csv, the 4 MB C3 layout)
    --fw-dir DIR         firmware build dir (default firmware/build)
    --no-firmware        only flash the content/index partitions
    --no-content         only flash the firmware + partition table

Only the meaningful bytes from content.bin/index.bin are written.  The rest
of each erased data partition remains 0xFF; sending multi-megabyte padded
images is unnecessary and made native-USB flashing less reliable.
"""

from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from archive_format import parse_partitions_csv, ArchiveError  # noqa: E402


def find_idf_python() -> str:
    env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env and os.path.exists(os.path.join(env, "bin", "python")):
        return os.path.join(env, "bin", "python")
    for d in sorted(glob.glob(os.path.expanduser("~/.espressif/python_env/*/bin/python"))):
        return d
    pio_python = os.path.expanduser("~/.platformio/penv/bin/python")
    if os.path.exists(pio_python):
        return pio_python
    return sys.executable  # last resort: esptool may not be importable


def firmware_paths(fw_dir: str) -> tuple[str, str, str]:
    """Accept either a native ESP-IDF or PlatformIO build directory."""
    idf = (
        os.path.join(fw_dir, "bootloader", "bootloader.bin"),
        os.path.join(fw_dir, "partition_table", "partition-table.bin"),
        os.path.join(fw_dir, "pocketwiki.bin"),
    )
    pio = (
        os.path.join(fw_dir, "bootloader.bin"),
        os.path.join(fw_dir, "partitions.bin"),
        os.path.join(fw_dir, "firmware.bin"),
    )
    return pio if os.path.exists(pio[2]) else idf


def main() -> int:
    ap = argparse.ArgumentParser(description="Flash PocketWiki firmware + content")
    ap.add_argument("-p", "--port", default=None)
    ap.add_argument("-b", "--baud", type=int, default=115200,
                    help="115200 is the reliable maximum over USB-Serial/JTAG")
    ap.add_argument("-c", "--chip", default="esp32c3",
                    help="esptool chip id, e.g. esp32c3 or esp32s3")
    ap.add_argument("-P", "--partitions",
                    default=os.path.join(ROOT, "firmware", "partitions.csv"),
                    help="partition layout describing the flash")
    ap.add_argument("--before", default="default_reset")
    ap.add_argument("--fw-dir", default=os.path.join(ROOT, "firmware", "build"))
    ap.add_argument("--no-firmware", action="store_true")
    ap.add_argument("--no-content", action="store_true")
    args = ap.parse_args()

    port = args.port
    if port is None:
        ports = glob.glob("/dev/cu.usbmodem*")
        if not ports:
            print("error: no port given and no /dev/cu.usbmodem* found", file=sys.stderr)
            return 1
        port = ports[0]
        print(f"using port {port}")

    parts = parse_partitions_csv(args.partitions)
    fw_dir = args.fw_dir

    images = []  # (offset, path)
    if not args.no_firmware:
        boot, ptable, app = firmware_paths(fw_dir)
        for path in (boot, ptable, app):
            if not os.path.exists(path):
                print(f"error: missing {path} — run `idf.py build` first", file=sys.stderr)
                return 1
        images.append((0x0, boot))
        images.append((0x8000, ptable))
        images.append((parts["factory"]["offset"], app))
        if os.path.getsize(app) > parts["factory"]["size"]:
            print(f"error: app image {os.path.getsize(app)} B exceeds factory partition "
                  f"{parts['factory']['size']} B", file=sys.stderr)
            return 1

    if not args.no_content:
        for name, fname in (("content", "content.bin"), ("index", "index.bin")):
            path = os.path.join(fw_dir, "content", fname)
            if not os.path.exists(path):
                print(f"error: missing {path} — run `idf.py build` first", file=sys.stderr)
                return 1
            size = os.path.getsize(path)
            if size > parts[name]["size"]:
                print(f"error: {fname} is {size} B but {name} partition is only "
                      f"{parts[name]['size']} B — shrink or repartition the content",
                      file=sys.stderr)
                return 1
            images.append((parts[name]["offset"], path))

    esptool = [find_idf_python(), "-m", "esptool", "--chip", args.chip,
               "--port", port, "--baud", str(args.baud),
               "--before", args.before, "write_flash"]
    for off, path in images:
        print(f"  {path} -> 0x{off:x}")
        esptool += [f"0x{off:x}", path]

    print("flashing...")
    return subprocess.call(esptool)


if __name__ == "__main__":
    sys.exit(main())
