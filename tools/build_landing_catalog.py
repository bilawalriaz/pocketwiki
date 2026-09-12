#!/usr/bin/env python3
"""Build the article-title data used by the PocketWiki landing page."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
from content_paths import ARTICLES, CATALOG
TITLE_RE = re.compile(r"^\s*#\s+(.+?)\s*$", re.MULTILINE)


def article_title(path: Path) -> str:
    """Read the title from the same H1 source used to build each pack."""
    text = path.read_text(encoding="utf-8")
    match = TITLE_RE.search(text)
    if not match:
        raise ValueError(f"{path}: article has no Markdown H1 title")
    return match.group(1).strip()


def build_catalog(source_path: Path) -> dict:
    source = json.loads(source_path.read_text(encoding="utf-8"))
    if source.get("schema") != 1 or not isinstance(source.get("packs"), list):
        raise ValueError("catalog source must use schema 1 and contain packs")

    packs = []
    for definition in source["packs"]:
        article_dir = ARTICLES / definition.get("dir", "starter")
        titles = [article_title(article_dir / filename) for filename in definition["articles"]]
        packs.append({
            "id": definition["id"],
            "name": definition["name"],
            "version": int(definition["version"]),
            "description": definition["description"],
            "articles": titles,
        })

    return {
        "schema": 1,
        "catalog_version": source["catalog_version"],
        "packs": packs,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=CATALOG)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    catalog = build_catalog(args.source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(catalog, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Wrote landing catalogue with {len(catalog['packs'])} packs and "
          f"{sum(len(pack['articles']) for pack in catalog['packs'])} article titles to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
