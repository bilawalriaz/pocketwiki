#!/usr/bin/env python3
"""Build and atomically publish the PocketWiki pack catalogue to Cloudflare."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess

from build_pack_catalog import ROOT, build_catalog
from content_paths import CATALOG

BUCKET = "pocketwiki-packs"


def wrangler(*args: str) -> None:
    subprocess.run(["npx", "wrangler", *args], cwd=ROOT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--deploy-worker", action="store_true",
                        help="also deploy the R2-serving Worker and custom-domain route")
    args = parser.parse_args()

    source = CATALOG
    output = ROOT / "dist" / "packs"
    catalog = build_catalog(source, output)
    asset = ROOT / "android" / "app" / "src" / "main" / "assets" / "pack_catalog.json"
    asset.parent.mkdir(parents=True, exist_ok=True)
    asset.write_bytes((output / "index.json").read_bytes())

    # Publish immutable pack objects first. The short-lived index is the
    # release pointer and is deliberately uploaded last.
    for pack in catalog["packs"]:
        filename = pack["url"].rsplit("/", 1)[-1]
        source_file = output / f"v{catalog['catalog_version']}" / filename
        wrangler("r2", "object", "put",
                 f"{BUCKET}/v{catalog['catalog_version']}/{filename}",
                 "--remote", "--file", str(source_file),
                 "--content-type", "application/octet-stream",
                 "--cache-control", "public, max-age=31536000, immutable")

    wrangler("r2", "object", "put", f"{BUCKET}/index.json",
             "--remote", "--file", str(output / "index.json"),
             "--content-type", "application/json; charset=utf-8",
             "--cache-control", "public, max-age=300")
    if args.deploy_worker:
        wrangler("deploy", "--config", "cloudflare/packs/wrangler.jsonc")
    print(f"Published {len(catalog['packs'])} packs and catalogue version {catalog['catalog_version']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
