#!/usr/bin/env python3
"""Import the latest UK-curriculum distillations into PocketWiki packs.

The source database is read-only.  Only completed, quality-gated lessons are
copied into ``articles/db-packs/uk-curriculum`` and the hand-curated catalogue
definitions are refreshed.  Pack metadata carries the Wikipedia/CC BY-SA
provenance, while the lesson body stays clean for reading.
"""

from __future__ import annotations

import argparse
import json
import re
import sqlite3
import unicodedata
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
from article_text import finalize_article
from content_paths import ARTICLES, CATALOG
DEFAULT_DB = Path.home() / "wiki-distill" / "educational-source.db"
PACK_DIR = ARTICLES / "db-packs" / "uk-curriculum"
DEFAULT_SINCE = "2026-09-10T20:52:00"
EXPECTED_COUNT = 67


PACKS = [
    {
        "id": "uk-curriculum-companion",
        "name": "UK Curriculum Knowledge Companion",
        "description": (
            "A compact knowledge companion spanning primary through secondary topics "
            "across England, Scotland, Wales, and Northern Ireland. It supports "
            "curriculum study but is not a replacement for official programmes of study."
        ),
        "titles": None,
    },
    {
        "id": "uk-literacy-arts-languages",
        "name": "UK Literacy, Arts & Languages",
        "description": (
            "Reading, writing, phonics, literary craft, music, visual art, and the "
            "languages of the UK for curriculum-linked exploration."
        ),
        "titles": [
            "Alliteration", "Blank verse", "Creative writing", "Free verse",
            "Goidelic languages", "Grapheme", "Irish language", "Literary device",
            "Modern art", "Musical instrument", "Onomatopoeia", "Orchestra",
            "Oxymoron", "Personification", "Persuasive writing", "Phonics",
            "Punctuation", "Reading comprehension", "Scottish Gaelic",
            "Welsh language",
        ],
    },
    {
        "id": "uk-maths-stem-computing",
        "name": "UK Maths, Science & Computing",
        "description": (
            "High-leverage mathematics, science, design technology, digital literacy, "
            "and computing concepts aligned with UK school knowledge spines."
        ),
        "titles": [
            "Binary code", "Design and Technology", "Euclidean vector", "Generator",
            "Life cycle", "Multiple (mathematics)", "Non-renewable resource",
            "Personal data", "Phishing", "Program (machine)", "Proportionality",
            "Projected coordinate system", "Rounding", "Series and parallel circuits",
            "Spectrum", "Topographic map", "Web browser", "Weight",
        ],
    },
    {
        "id": "uk-britain-civic-life",
        "name": "Britain, Nations & Civic Life",
        "description": (
            "UK history, the four nations, citizenship, democracy, wellbeing, and "
            "everyday institutions for context-rich study."
        ),
        "titles": [
            "Consent", "Curriculum for Excellence", "Election", "Elizabethan era",
            "Historical source", "History of Northern Ireland", "History of Scotland",
            "History of Wales", "Humanism", "Magna Carta", "Migration", "Morality",
            "Northern Ireland", "Northern Ireland Curriculum", "Ordnance Survey",
            "Outline of ancient China", "Outline of ancient India",
            "Parliament of the United Kingdom", "Pension", "Physical education",
            "Relationship quality", "Roman Britain", "Scotland", "Settlement geography",
            "Team sport", "Tudor period", "Victorian era", "Wales", "Well-being",
        ],
    },
]


def slugify(title: str) -> str:
    folded = "".join(
        c for c in unicodedata.normalize("NFKD", title)
        if not unicodedata.combining(c)
    )
    return re.sub(r"[^a-z0-9]+", "-", folded.lower()).strip("-")


def normalize_lesson(title: str, draft: str) -> str:
    text = (draft or "").replace("\r\n", "\n").strip()
    if text.startswith("```"):
        text = re.sub(r"^```(?:markdown|md)?\s*", "", text, flags=re.I)
        text = re.sub(r"\s*```$", "", text).strip()
    text = re.sub(r"^#\s+[^\n]+", f"# {title}", text, count=1)
    if not text.startswith(f"# {title}\n") and text != f"# {title}":
        raise ValueError(f"lesson for {title!r} does not begin with its title")
    return text.rstrip()


def read_articles(db_path: Path, since: str) -> list[dict]:
    with sqlite3.connect(f"file:{db_path.resolve()}?mode=ro", uri=True) as db:
        db.row_factory = sqlite3.Row
        rows = db.execute(
            """
            SELECT a.id, a.title, a.created_at, d.draft, d.model
            FROM articles AS a
            JOIN minimax_distillations AS d ON d.article_id = a.id
            WHERE a.created_at >= ?
              AND d.status = 'complete'
              AND d.quality_status = 'complete'
              AND length(trim(COALESCE(d.draft, ''))) > 100
            ORDER BY a.id
            """,
            (since,),
        ).fetchall()
    if len(rows) != EXPECTED_COUNT:
        raise RuntimeError(
            f"expected {EXPECTED_COUNT} completed new articles since {since}, found {len(rows)}"
        )
    return [dict(row) for row in rows]


def write_articles(rows: list[dict]) -> tuple[dict[str, str], set[str]]:
    PACK_DIR.mkdir(parents=True, exist_ok=True)
    files: dict[str, str] = {}
    changed: set[str] = set()
    for row in rows:
        filename = f"{int(row['id']):05d}-{slugify(row['title'])}.md"
        path = PACK_DIR / filename
        content, _ = finalize_article(normalize_lesson(row["title"], row["draft"]))
        if not path.exists() or path.read_text(encoding="utf-8") != content:
            path.write_text(content, encoding="utf-8")
            changed.add(filename)
        files[row["title"]] = filename
    expected = set(files.values())
    for path in PACK_DIR.glob("*.md"):
        if path.name not in expected:
            path.unlink()
    (PACK_DIR / "selection.json").write_text(
        json.dumps(
            {
                "source": "wiki-distill educational-source.db",
                "articles": [
                    {"id": int(row["id"]), "title": row["title"], "model": row["model"]}
                    for row in rows
                ],
            },
            indent=2,
            ensure_ascii=False,
        )
        + "\n",
        encoding="utf-8",
    )
    return files, changed


def refresh_catalog(files: dict[str, str], changed: set[str], requested_version: int) -> None:
    source = json.loads(CATALOG.read_text(encoding="utf-8"))
    old = {pack["id"]: pack for pack in source["packs"]}
    managed = {pack["id"] for pack in PACKS}
    source["packs"] = [pack for pack in source["packs"] if pack["id"] not in managed]
    for definition in PACKS:
        titles = list(files) if definition["titles"] is None else definition["titles"]
        missing = [title for title in titles if title not in files]
        if missing:
            raise RuntimeError(f"{definition['id']} has unknown titles: {missing}")
        articles = [files[title] for title in titles]
        previous = old.get(definition["id"])
        version = int(previous["version"]) if previous else 1
        if previous and (previous.get("articles") != articles or
                         any(filename in changed for filename in articles)):
            version += 1
        source["packs"].append(
            {
                "id": definition["id"],
                "name": definition["name"],
                "version": version,
                "description": definition["description"],
                "dir": "db-packs/uk-curriculum",
                "articles": articles,
            }
        )
    source["catalog_version"] = max(int(source["catalog_version"]), requested_version)
    CATALOG.write_text(json.dumps(source, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--db", type=Path, default=DEFAULT_DB)
    parser.add_argument("--since", default=DEFAULT_SINCE)
    parser.add_argument("--catalog-version", type=int, default=4)
    args = parser.parse_args()
    rows = read_articles(args.db, args.since)
    files, changed = write_articles(rows)
    refresh_catalog(files, changed, args.catalog_version)
    print(json.dumps({"articles": len(rows), "packs": [pack["id"] for pack in PACKS]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
