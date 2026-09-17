#!/usr/bin/env python3
"""Publish verified PocketWiki firmware images to the R2 bucket.

The browser flasher reads ``firmware/index.json`` as the release pointer. Image
objects live below an immutable release directory, so a failed upload cannot
make the page point at a mixed set of binaries.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess

from stage_web_firmware import DEFAULT_DIST, ROOT, stage

BUCKET = "pocketwiki-packs"
ENV_BY_ID = {"esp32c3": "esp32-c3", "esp32s3": "esp32-s3"}


def wrangler(*args: str) -> None:
    subprocess.run(["npx", "wrangler", *args], cwd=ROOT, check=True)


def release_id(manifest: dict) -> str:
    digest = hashlib.sha256(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()[:12]
    date = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    return f"{date}-{digest}"


def main() -> int:
    parser = argparse.ArgumentParser(description="Publish PocketWiki firmware to R2")
    parser.add_argument("--dist", type=Path, default=DEFAULT_DIST)
    parser.add_argument("--release", help="explicit immutable release directory name")
    args = parser.parse_args()

    manifest = stage(
        args.dist,
        ROOT / "cloudflare" / "site" / "public" / "pocketwiki" / "fw",
        check_only=True,
    )
    release = args.release or release_id(manifest)
    published = dict(manifest, release=release, source="r2")
    published["targets"] = []

    for target in manifest["targets"]:
        target_copy = dict(target, images=[])
        published["targets"].append(target_copy)
        for image in target["images"]:
            image_copy = dict(image)
            image_copy["path"] = f"firmware/{release}/{image['path']}"
            target_copy["images"].append(image_copy)

            source = args.dist / ENV_BY_ID[target["id"]] / image["path"].split("/", 1)[1]
            if not source.is_file():
                raise FileNotFoundError(source)
            wrangler(
                "r2", "object", "put", f"{BUCKET}/{image_copy['path']}",
                "--remote", "--file", str(source),
                "--content-type", "application/octet-stream",
                "--cache-control", "public, max-age=31536000, immutable",
            )

    manifest_path = ROOT / "build" / "firmware-r2-index.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(
        json.dumps(published, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    wrangler(
        "r2", "object", "put", f"{BUCKET}/firmware/index.json",
        "--remote", "--file", str(manifest_path),
        "--content-type", "application/json; charset=utf-8",
        "--cache-control", "public, max-age=300, stale-while-revalidate=3600",
    )
    print(f"Published firmware release {release} and latest pointer to R2")
    print("Manifest: https://packs.educated.space/firmware/index.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
