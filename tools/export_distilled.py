#!/usr/bin/env python3
"""Export done distillations from the wiki-distill database into
articles/distilled/ and regenerate the distilled packs in
packs/catalog-source.json.

The wiki-distill repo (github.com/bilawalriaz/wiki-distill) stores one
distillation per vital article in SQLite. This tool:

  1. selects distillations with status='done' and a usable draft, excluding
     only critic-failed rows (critic_ok=0); critic-passing and never-criticised
     (critic_ok IS NULL) drafts are both included
  2. writes each as articles/distilled/<id>-<slug>.md: H1 title, markdown
     tables rendered as bullets (no per-article Source/Licence footer)
  3. assigns Level 4 articles to their Wikipedia vital category using cached
     category link lists (tools/l4_vital_categories.json, refresh with
     --refresh-categories)
  4. regenerates the "wikipedia-vital*" packs in packs/catalog-source.json as
     ten coarse packs: Levels 1-3 (2 parts), People (3 parts), Geography (2
     parts) and History & Culture (2 parts), split so every .pwp stays under
     the 2 MiB device upload limit. Versions bump automatically when a pack's
     article list changes, keeping the immutable R2 URLs fresh.

Starter packs and every other catalog field are left untouched, so the source
stays hand-curatable. Re-run on a newer database copy to extend the packs;
keep existing pack ids stable and bump their versions.

Usage:
  python3 tools/export_distilled.py [--db PATH] [--refresh-categories]
"""

from __future__ import annotations

import argparse
import json
import re
import sqlite3
import sys
import time
import unicodedata
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

from content_paths import ARTICLES, CATALOG
DEFAULT_DB = Path.home() / "wiki-distill" / "wiki-distill.db"
ARTICLES_DIR = ARTICLES / "distilled"
CATALOG_PATH = CATALOG
CATEGORY_CACHE = ROOT / "tools" / "l4_vital_categories.json"

# Hard device limit enforced by build_pack_catalog.py (Android/firmware).
MAX_PACK_BYTES = 2_097_152
# Safety margin below the hard limit; the build validates the real bytes.
TARGET_PACK_BYTES = 1_900_000

# One distillation per vital article; ids are the original queue ranks.
L1_MAX, L2_MAX, L3_MAX = 10, 100, 999

L4_CATEGORIES = [
    "People", "History", "Geography", "Arts", "Philosophy and religion",
    "Everyday life", "Society and social sciences", "Biology and health sciences",
    "Physical sciences", "Mathematics", "Technology", "Other",
]

UA = {"User-Agent": "pocketwiki-export/1.0 (categorization; contact: none)"}
API = "https://en.wikipedia.org/w/api.php"

ATTRIBUTION = ""  # article pages carry no per-article Source/Licence footer


def slugify(title: str) -> str:
    # ASCII-fold accents so "Albrecht Dürer" -> "albrecht-durer".
    folded = "".join(c for c in unicodedata.normalize("NFKD", title)
                     if not unicodedata.combining(c))
    return re.sub(r"[^a-z0-9]+", "-", folded.lower()).strip("-")


def split_table_row(line: str) -> list[str]:
    """Split a markdown table row on '|' outside $...$ math spans."""
    cells: list[str] = []
    buf: list[str] = []
    in_math = False
    for ch in line:
        if ch == "$":
            in_math = not in_math
            buf.append(ch)
        elif ch == "|" and not in_math:
            cells.append("".join(buf).strip())
            buf = []
        else:
            buf.append(ch)
    cells.append("".join(buf).strip())
    return cells


_SEPARATOR = re.compile(r":?-+:?")


def tables_to_bullets(text: str) -> str:
    """Render markdown pipe tables as '- **label** — value' bullets.

    A block is a table when it has a header row followed by a separator row
    (cells of dashes/colons); otherwise every '|' row is still converted so no
    raw pipes reach the device renderer.
    """
    lines = text.split("\n")
    out: list[str] = []
    i, n = 0, len(lines)
    while i < n:
        if not lines[i].startswith("|"):
            out.append(lines[i])
            i += 1
            continue
        block: list[str] = []
        while i < n and lines[i].startswith("|"):
            block.append(lines[i])
            i += 1
        sep_idx = next(
            (j for j, ln in enumerate(block)
             if all(_SEPARATOR.fullmatch(c) for c in split_table_row(ln) if c)),
            None,
        )
        rows = block[sep_idx + 1:] if sep_idx is not None else block
        for ln in rows:
            cells = [c for c in split_table_row(ln) if c]
            if not cells:
                continue
            out.append(f"- **{cells[0]}** — {' '.join(cells[1:])}")
    return "\n".join(out)


def render_article(title: str, draft: str) -> str:
    body = tables_to_bullets(draft).rstrip()
    url = "https://en.wikipedia.org/wiki/" + title.replace(" ", "_")
    return f"# {title}\n\n{body}{ATTRIBUTION.format(title=title, url=url)}"


def select_distillations(db_path: Path) -> list[dict]:
    con = sqlite3.connect(db_path)
    rows = con.execute(
        """
        SELECT a.id, a.title, d.draft
        FROM (
            SELECT article_id, draft,
                   ROW_NUMBER() OVER (
                       PARTITION BY article_id
                       ORDER BY (critic_ok IS NOT NULL) DESC, critic_ok DESC, created_at DESC
                   ) AS rn
            FROM distillations
            WHERE status = 'done' AND (critic_ok = 1 OR critic_ok IS NULL)
        ) d
        JOIN articles a ON a.id = d.article_id
        WHERE d.rn = 1
        ORDER BY a.id
        """
    ).fetchall()
    con.close()
    articles = []
    for aid, title, draft in rows:
        if not draft or len(draft.strip()) < 150:
            print(f"  skip {aid:04d} {title!r}: unusable draft ({len(draft or '')} bytes)")
            continue
        articles.append({"id": aid, "title": title, "draft": draft})
    return articles


def fetch_category_links(page: str) -> list[str]:
    url = API + "?" + urllib.parse.urlencode({
        "action": "parse", "page": page, "prop": "links",
        "format": "json", "formatversion": "2", "redirects": "1",
    })
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = json.load(resp)
    return [l["title"] for l in data["parse"]["links"] if l.get("ns") == 0]


def load_categories(refresh: bool) -> dict[str, list[str]]:
    if CATEGORY_CACHE.exists() and not refresh:
        return json.loads(CATEGORY_CACHE.read_text(encoding="utf-8"))
    categories: dict[str, list[str]] = {}
    for name in L4_CATEGORIES:
        try:
            links = fetch_category_links(f"Wikipedia:Vital articles/Level 4/{name}")
            categories[name] = links
            print(f"  fetched {name}: {len(links)} links")
        except Exception as exc:  # page may not exist; other failures warn
            print(f"  WARNING: could not fetch Level 4/{name}: {exc}")
        time.sleep(0.4)
    CATEGORY_CACHE.write_text(
        json.dumps(categories, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    return categories


def level_of(aid: int) -> int:
    if aid <= L1_MAX:
        return 1
    if aid <= L2_MAX:
        return 2
    if aid <= L3_MAX:
        return 3
    return 4


def category_of(aid: int, title: str, categories: dict[str, list[str]]) -> str | None:
    if level_of(aid) != 4:
        return None
    for name in L4_CATEGORIES:
        if title in categories.get(name, []):
            return name
    return None


def chunk(articles: list[dict], est_bytes: int) -> list[list[dict]]:
    """Split into balanced chunks so estimated .pwp size stays in budget."""
    if not articles:
        return []
    total = sum(est_bytes) or 1
    k = max(1, -(-total // TARGET_PACK_BYTES))
    size = -(-len(articles) // k)
    return [articles[i:i + size] for i in range(0, len(articles), size)]


_COMP_RATIO = 0.55  # compressed content.bin / rendered markdown (measured 0.49-0.53)


def estimate_bytes(articles: list[dict]) -> int:
    """Estimated .pwp bytes; empirically compressed archive ≈ 0.52 × markdown."""
    md = sum(len(a["draft"]) + len(a["title"]) + 300 for a in articles)
    return int(md * _COMP_RATIO) + 200 * len(articles)


def pack_def(name: str, pack_id: str, version: int, description: str,
             articles: list[dict]) -> dict:
    return {
        "id": pack_id,
        "name": name,
        "version": version,
        "dir": "distilled",
        "description": description,
        "articles": [f"{a['id']:03d}-{slugify(a['title'])}.md" for a in articles],
    }


def build_packs(articles: list[dict], categories: dict[str, list[str]]) -> list[dict]:
    """Group into 10 coarse packs: Levels 1-3, People, Geography, History & Culture."""
    l1l2 = [a for a in articles if level_of(a["id"]) <= 2]
    l3 = [a for a in articles if level_of(a["id"]) == 3]
    l4_by_cat: dict[str, list[dict]] = {}
    unassigned: list[dict] = []
    for a in articles:
        if level_of(a["id"]) != 4:
            continue
        cat = category_of(a["id"], a["title"], categories)
        if cat is None:
            unassigned.append(a)
        else:
            l4_by_cat.setdefault(cat, []).append(a)
    if unassigned:
        print(f"  WARNING: {len(unassigned)} Level 4 articles matched no category: "
              + ", ".join(a["title"] for a in unassigned[:5]))

    packs: list[dict] = []

    def add_parts(name: str, stem: str, version: int, group: list[dict],
                  description_fmt: str) -> None:
        chunks = chunk(group, [estimate_bytes([a]) for a in group])
        for i, part in enumerate(chunks, 1):
            packs.append(pack_def(
                f"{name} (Part {i} of {len(chunks)})" if len(chunks) > 1 else name,
                f"{stem}-{i}", version,
                description_fmt.format(i=i, n=len(chunks), count=len(part)), part))

    add_parts("Wikipedia Vital: Levels 1-3", "wikipedia-vital-levels-1-3", 1,
              l1l2 + l3,
              "Study-grade distillations of the Wikipedia vital Levels 1-3 "
              "(part {i} of {n}, {count} articles).")
    add_parts("Wikipedia Vital: People", "wikipedia-vital-people", 2,
              l4_by_cat.get("People", []),
              "Wikipedia vital Level 4 people distillations "
              "(part {i} of {n}, {count} articles).")
    add_parts("Wikipedia Vital: Geography", "wikipedia-vital-geography", 2,
              l4_by_cat.get("Geography", []),
              "Wikipedia vital Level 4 geography distillations "
              "(part {i} of {n}, {count} articles).")
    add_parts("Wikipedia Vital: History & Culture", "wikipedia-vital-history-and-culture", 1,
              l4_by_cat.get("History", []) + l4_by_cat.get("Arts", []),
              "Wikipedia vital Level 4 history and arts distillations "
              "(part {i} of {n}, {count} articles).")
    return packs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--db", type=Path, default=DEFAULT_DB, help="path to wiki-distill.db")
    ap.add_argument("--refresh-categories", action="store_true",
                    help="refetch the Level 4 category link lists from Wikipedia")
    args = ap.parse_args()

    if not args.db.exists():
        print(f"database not found: {args.db}", file=sys.stderr)
        return 1

    articles = select_distillations(args.db)
    print(f"exported distillations: {len(articles)}")

    categories = load_categories(args.refresh_categories)
    for a in articles:
        a["category"] = category_of(a["id"], a["title"], categories)

    ARTICLES_DIR.mkdir(parents=True, exist_ok=True)
    written = changed = unchanged = 0
    seen_slugs: dict[str, int] = {}
    for a in articles:
        slug = slugify(a["title"])
        if slug in seen_slugs:
            print(f"  ERROR: slug collision: {seen_slugs[slug]} and {a['id']} -> {slug}.md",
                  file=sys.stderr)
            return 1
        seen_slugs[slug] = a["id"]
        path = ARTICLES_DIR / f"{a['id']:03d}-{slug}.md"
        content = render_article(a["title"], a["draft"])
        if path.exists() and path.read_text(encoding="utf-8") == content:
            unchanged += 1
        elif path.exists():
            path.write_text(content, encoding="utf-8")
            changed += 1
        else:
            path.write_text(content, encoding="utf-8")
            written += 1
    expected = {f"{a['id']:03d}-{slugify(a['title'])}.md" for a in articles}
    pruned = []
    for path in ARTICLES_DIR.glob("*.md"):
        if re.match(r"^\d{3,}-", path.name) and path.name not in expected:
            path.unlink()
            pruned.append(path.name)
    if pruned:
        print(f"pruned {len(pruned)} stale files, e.g. {pruned[:5]}")
    print(f"articles: {written} new, {changed} changed, {unchanged} unchanged")

    # Regenerate only the distilled packs in the catalog source.
    source = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
    old_distilled = {
        p["id"]: p for p in source["packs"]
        if p["id"] == "wikipedia-vital" or p["id"].startswith("wikipedia-vital-")
    }
    source["packs"] = [p for p in source["packs"] if p["id"] not in old_distilled]
    for pack in build_packs(articles, categories):
        old = old_distilled.get(pack["id"])
        # Same id, changed contents -> bump version so the immutable R2 URL changes.
        if old is not None and old.get("articles") != pack["articles"]:
            pack["version"] = int(old["version"]) + 1
        source["packs"].append(pack)
    CATALOG_PATH.write_text(
        json.dumps(source, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"catalog source: {len(source['packs'])} packs "
          f"({sum(len(p['articles']) for p in source['packs'])} articles)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
