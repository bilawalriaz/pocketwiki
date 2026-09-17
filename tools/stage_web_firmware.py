#!/usr/bin/env python3
"""Stage firmware images and a flash manifest for the landing page flasher.

The landing page flashes a board straight over USB with esptool-js (Web
Serial), so it needs the same images the CLI flasher writes, at the same
offsets, from the same partition tables. This tool copies those images out of
``firmware/dist/<env>/`` into the landing page's deployment bundle and writes
``fw/firmware.json`` describing where each image goes.

    pio run -d firmware -e esp32-c3
    pio run -d firmware -e esp32-s3
    python3 tools/stage_web_firmware.py
    sh cloudflare/site/prepare.sh
    wrangler deploy --config cloudflare/site/wrangler.jsonc

Every offset comes from the staged ``partitions.csv`` and every image is
checked against the partition it lands in, so a layout change in
``firmware/partitions*.csv`` cannot silently produce a bad flash plan. The
manifest is the single source the browser reads at flash time: offsets, byte
counts, and SHA-256 digests the page verifies after fetching.

Only the meaningful bytes of each image are staged. ``flash_all.py`` does the
same: the tail of an erased partition is already 0xFF.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from archive_format import ArchiveError, parse_partitions_csv  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DIST = ROOT / "firmware" / "dist"
DEFAULT_OUT = ROOT / "cloudflare" / "site" / "public" / "pocketwiki" / "fw"

# The bootloader always runs from the start of flash and the partition table
# from the second sector: fixed by the chip, not by the CSV.
BOOTLOADER_OFFSET = 0x0
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_LIMIT = 0x1000

MIB = 1024 * 1024

# (label shown while flashing, path under the dist dir, offset source)
# An offset source is either an int or a partition name in partitions.csv.
IMAGE_SPECS = (
    ("Bootloader", "bootloader.bin", BOOTLOADER_OFFSET),
    ("Partition table", "partitions.bin", PARTITION_TABLE_OFFSET),
    ("Firmware", "firmware.bin", "factory"),
    ("Starter library", "content/content.bin", "content"),
    ("Library index", "content/index.bin", "index"),
)

TARGETS = (
    dict(id="esp32c3", env="esp32-c3", chip="ESP32-C3", label="ESP32-C3 · 4 MB"),
    dict(id="esp32s3", env="esp32-s3", chip="ESP32-S3", label="ESP32-S3 · 16 MB"),
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def flash_size_string(bytes_: int) -> str:
    """Return the esptool flash size name for a whole-MiB flash region."""
    if bytes_ % MIB or bytes_ // MIB not in (1, 2, 4, 8, 16, 32, 64, 128):
        raise ArchiveError(f"flash region of {bytes_} bytes is not a supported flash size")
    return f"{bytes_ // MIB}MB"


def image_limit(rel: str, source, partitions: dict) -> int:
    """Bytes available to an image: a fixed sector, or its CSV partition."""
    if rel == "bootloader.bin":
        return PARTITION_TABLE_OFFSET - BOOTLOADER_OFFSET
    if rel == "partitions.bin":
        return PARTITION_TABLE_LIMIT
    return partitions[source]["size"]


def build_target(dist: Path, target: dict) -> dict:
    """Read one staged environment and return its manifest target entry."""
    env_dir = dist / target["env"]
    csv_path = env_dir / "partitions.csv"
    if not csv_path.is_file():
        raise ArchiveError(f"{csv_path} is missing: build {target['env']} first (pio run -d firmware -e {target['env']})")

    partitions = parse_partitions_csv(str(csv_path))
    for name in ("nvs", "factory", "content", "index", "packs"):
        if name not in partitions:
            raise ArchiveError(f"{csv_path}: no {name} partition")

    images = []
    for label, rel, source in IMAGE_SPECS:
        path = env_dir / rel
        if not path.is_file():
            raise ArchiveError(f"{path} is missing: build {target['env']} first (pio run -d firmware -e {target['env']})")
        data = path.read_bytes()
        if not data:
            raise ArchiveError(f"{path} is empty")
        offset = source if isinstance(source, int) else partitions[source]["offset"]
        limit = image_limit(rel, source, partitions)
        if len(data) > limit:
            raise ArchiveError(
                f"{rel} is {len(data)} bytes and does not fit the {limit}-byte region at 0x{offset:x} in {csv_path}")
        images.append(dict(name=label, offset=offset, path=f"{target['id']}/{rel}", bytes=len(data), sha256=sha256(data)))

    staged = sorted(images, key=lambda image: image["offset"])
    for image, following in zip(staged, staged[1:]):
        if image["offset"] + image["bytes"] > following["offset"]:
            raise ArchiveError(f"{image['path']} overlaps {following['path']}")

    flash_bytes = max(part["offset"] + part["size"] for part in partitions.values())
    nvs = partitions["nvs"]
    digest = hashlib.sha256(b"".join(bytes.fromhex(image["sha256"]) for image in images)).hexdigest()[:12]

    return dict(
        id=target["id"],
        chip=target["chip"],
        label=target["label"],
        flash_size=flash_size_string(flash_bytes),
        flash_bytes=flash_bytes,
        digest=digest,
        nvs=dict(offset=nvs["offset"], size=nvs["size"]),
        images=images,
    )


def git_version() -> str | None:
    try:
        out = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"],
                             cwd=ROOT, capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.stdout.strip() or None


def stage(dist: Path, out: Path, targets=TARGETS, check_only: bool = False) -> dict:
    """Validate the staged images, copy them into the web bundle, write the manifest."""
    manifest = dict(
        schema=1,
        version=git_version(),
        generated=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        targets=[],
    )
    for target in targets:
        entry = build_target(dist, target)
        manifest["targets"].append(entry)
        if check_only:
            continue
        env_dir = dist / target["env"]
        for image in entry["images"]:
            rel = image["path"].split("/", 1)[1]
            destination = out / target["id"] / rel
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(env_dir / rel, destination)
    if not check_only:
        out.mkdir(parents=True, exist_ok=True)
        (out / "firmware.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="Stage firmware images and a flash manifest for the web flasher")
    parser.add_argument("--dist", type=Path, default=DEFAULT_DIST, help=f"staged image root (default {DEFAULT_DIST})")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help=f"web bundle directory (default {DEFAULT_OUT})")
    parser.add_argument("--check", action="store_true", help="validate the staged images without writing anything")
    args = parser.parse_args()

    try:
        manifest = stage(args.dist, args.out, check_only=args.check)
    except ArchiveError as error:
        print(f"stage_web_firmware: {error}", file=sys.stderr)
        return 1

    for target in manifest["targets"]:
        total = sum(image["bytes"] for image in target["images"])
        print(f"{target['id']}: {target['chip']} {target['flash_size']} · {len(target['images'])} images, {total} bytes · {target['digest']}")
    if args.check:
        print(f"stage_web_firmware: {args.dist} is flashable (nothing written)")
    else:
        print(f"stage_web_firmware: wrote {args.out / 'firmware.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
