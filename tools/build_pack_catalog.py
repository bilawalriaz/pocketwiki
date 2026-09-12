#!/usr/bin/env python3
"""Build reproducible example .pwp files and their public catalogue."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
from content_paths import ARTICLES, CATALOG
sys.path.insert(0, str(ROOT / "tools"))

import archive_format as af  # noqa: E402
import pack_content  # noqa: E402

MAX_PACK_BYTES = 2_097_152


def build_catalog(source_path: Path, output_dir: Path) -> dict:
    source = json.loads(source_path.read_text(encoding="utf-8"))
    if source.get("schema") != 1 or not isinstance(source.get("packs"), list):
        raise ValueError("catalog source must use schema 1 and contain packs")

    base_url = source["base_url"].rstrip("/")
    version_dir = output_dir / f"v{source['catalog_version']}"
    version_dir.mkdir(parents=True, exist_ok=True)
    built = []
    seen_ids: set[str] = set()

    for definition in source["packs"]:
        pack_id = definition["id"]
        if pack_id in seen_ids or not pack_id.replace("-", "").isalnum():
            raise ValueError(f"invalid or duplicate pack id: {pack_id}")
        seen_ids.add(pack_id)
        version = int(definition["version"])
        filename = f"{pack_id}-v{version}.pwp"
        pack_path = version_dir / filename

        with tempfile.TemporaryDirectory(prefix=f"pocketwiki-{pack_id}-") as temp:
            article_dir = Path(temp) / "articles"
            build_dir = Path(temp) / "build"
            article_dir.mkdir()
            articles_dir = definition.get("dir", "starter")
            for article in definition["articles"]:
                source_article = ARTICLES / articles_dir / article
                if not source_article.is_file():
                    raise FileNotFoundError(source_article)
                shutil.copy2(source_article, article_dir / source_article.name)
            # Gzip has bounded, article-sized decode memory and is the default
            # distribution codec so the same catalogue is reliable on C3 and S3.
            manifest = pack_content.build(str(article_dir), str(build_dir), codec="gzip")
            content = (build_dir / "content.bin").read_bytes()
            index = (build_dir / "index.bin").read_bytes()
            pack_path.write_bytes(af.pack_pack_file(content, index))

        data = pack_path.read_bytes()
        content, index = af.unpack_pack_file(data)
        archive = af.Archive(content, index)
        if len(data) > MAX_PACK_BYTES:
            raise ValueError(f"{filename} exceeds Android/firmware upload limit")
        if archive.count != manifest["article_count"]:
            raise ValueError(f"{filename} article count changed during wrapping")

        built.append({
            "id": pack_id,
            "name": definition["name"],
            "version": version,
            "description": definition["description"],
            "articles": archive.count,
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "url": f"{base_url}/v{source['catalog_version']}/{filename}",
            "license": "CC BY-SA 4.0",
        })

    catalog = {
        "schema": 1,
        "catalog_version": source["catalog_version"],
        "generated_at": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "packs": built,
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "index.json").write_text(
        json.dumps(catalog, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return catalog


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=CATALOG)
    parser.add_argument("--output", type=Path, default=ROOT / "dist" / "packs")
    parser.add_argument("--android-asset", type=Path,
                        default=ROOT / "android" / "app" / "src" / "main" / "assets" / "pack_catalog.json")
    args = parser.parse_args()
    catalog = build_catalog(args.source, args.output)
    args.android_asset.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.output / "index.json", args.android_asset)
    print(json.dumps(catalog, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
