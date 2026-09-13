#!/usr/bin/env python3
"""Remove generator commentary using the spans a model verified verbatim.

``tools/clean_distill_db.py`` removes commentary the deterministic rules
recognise. This tool handles the rest: cases a model found that no rule covers
yet, where a broad rule would match ordinary prose (a rule matching
"changes made" also matched "Two changes made the katana dominant").

So the unit of removal here is not a pattern but a *verified quote*. The audit
report records, for each article, spans the model copied exactly and that were
located in the text, with offsets. Each span is expanded to the block of lines
containing it, overlapping blocks are merged, and anything containing a
markdown heading is refused -- a heading means the block is article structure,
not a note.

    # Review every proposed removal. Nothing is written.
    python3 tools/apply_audit_removals.py --audit /tmp/meta-confirm.jsonl

    # Apply, recording cleanup_audit rows.
    python3 tools/apply_audit_removals.py --audit /tmp/meta-confirm.jsonl --apply
"""

from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import os
import sqlite3
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from article_text import MAX_REMOVAL_FRACTION, MIN_REMOVAL_LIMIT, TRAILER_LABEL  # noqa: E402

DEFAULT_DB = Path(os.environ.get(
    "POCKETWIKI_DISTILL_DB", Path.home() / "wiki-distill" / "educational-source.db"))
FIELD = "minimax_distillations.draft"
MODEL = "quote-anchored"
PROMPT_VERSION = "meta-cleanup-quote-v1"
HEADING = ("#",)
SEPARATORS = ("", "---", "***", "___", "```")


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _line_starts(text: str) -> list[int]:
    starts, pos = [0], 0
    for line in text.splitlines(keepends=True):
        pos += len(line)
        starts.append(pos)
    return starts


def _block(lines: list[str], index: int) -> tuple[int, int]:
    """The maximal run of non-blank lines containing ``index``."""
    start = end = index
    while start > 0 and lines[start - 1].strip():
        start -= 1
    while end + 1 < len(lines) and lines[end + 1].strip():
        end += 1
    return start, end


def propose(text: str, quotes: list[str]) -> dict | None:
    """Work out the block(s) to remove for one article, or ``None`` to refuse."""
    lines = text.splitlines()
    starts = _line_starts(text)
    blocks: list[tuple[int, int]] = []
    unlocated = []
    for quote in quotes:
        at = text.find(quote)
        if at == -1:
            unlocated.append(quote[:60])
            continue
        line = bisect.bisect_right(starts, at) - 1
        blocks.append(_block(lines, line))
    if not blocks:
        return None

    merged: list[list[int]] = []
    for start, end in sorted(blocks):
        if merged and start <= merged[-1][1] + 1:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])

    # A heading inside a block means the block is article structure.
    for start, end in merged:
        for line in lines[start:end + 1]:
            if line.lstrip().startswith(HEADING):
                return {"refused": "block contains a heading", "block": lines[start][:90]}

    # Take the separator, fence or label that introduced each block with it.
    for pair in merged:
        while pair[0] > 0 and (lines[pair[0] - 1].strip() in SEPARATORS
                               or TRAILER_LABEL.match(lines[pair[0] - 1])):
            pair[0] -= 1

    drop = set()
    for start, end in merged:
        drop.update(range(start, end + 1))
    kept = "\n".join(line for i, line in enumerate(lines) if i not in drop).rstrip() + "\n"
    removed_lines = [line for i, line in enumerate(lines) if i in drop and line.strip()]
    removed_chars = len(text) - len(kept)
    limit = max(MIN_REMOVAL_LIMIT, MAX_REMOVAL_FRACTION * len(text))
    if removed_chars > limit:
        return {"refused": f"removal {removed_chars} exceeds limit {limit:.0f}",
                "block": removed_lines[0][:90] if removed_lines else ""}
    return {"kept": kept, "removed_lines": removed_lines,
            "removed_chars": removed_chars, "unlocated": unlocated}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--audit", type=Path, required=True,
                        help="audit report jsonl whose spans are verified quotes")
    parser.add_argument("--db", type=Path, default=DEFAULT_DB)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--exclude",
                        help="comma-separated article ids to leave alone: model flags "
                             "that review showed are article content, not narration")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)

    con = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    drafts = dict(con.execute("SELECT article_id, COALESCE(draft,'') FROM minimax_distillations"))
    titles = dict(con.execute("SELECT article_id, title FROM minimax_distillations"))
    already = {r[0] for r in con.execute(
        "SELECT article_id FROM cleanup_audit WHERE prompt_version=? AND changed=1",
        (PROMPT_VERSION,))}
    con.close()
    excluded = {int(x) for x in args.exclude.split(",")} if args.exclude else set()

    proposals, refused = [], []
    for line in args.audit.read_text().splitlines():
        if not line.strip():
            continue
        row = json.loads(line)
        if not row.get("model_contaminated"):
            continue
        aid = int(Path(row["path"]).stem)
        quotes = [s["quote"] for s in (row.get("spans") or []) if s.get("verified")]
        quotes = [q for q in quotes if len(q.strip()) >= 8]
        if not quotes or aid in already or aid in excluded:
            continue
        text = drafts.get(aid, "")
        result = propose(text, quotes)
        if result is None:
            continue
        if "refused" in result:
            refused.append((aid, titles.get(aid, "?"), result))
            continue
        proposals.append({"article_id": aid, "title": titles.get(aid, "?"),
                          "original": text, **result})

    total = sum(p["removed_chars"] for p in proposals)
    print(f"proposed removals: {len(proposals)}   refused: {len(refused)}   "
          f"characters: {total:,}")
    for p in sorted(proposals, key=lambda x: -x["removed_chars"]):
        print(f"  -{p['removed_chars']:5d}  id={p['article_id']:6d} {p['title'][:34]:36s} "
              f"{p['removed_lines'][0][:70]!r}")
    for aid, title, result in refused:
        print(f"  REFUSED id={aid} {title[:34]}: {result['refused']}")

    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(
            [{k: v for k, v in p.items() if k != "original"} for p in proposals],
            indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    if not args.apply or not proposals:
        print("\ndry run: nothing written (use --apply)" if proposals else "\nnothing to do")
        return 0

    con = sqlite3.connect(args.db)
    run_id = str(uuid.uuid4())
    try:
        con.execute("BEGIN")
        now = datetime.now(timezone.utc).isoformat(timespec="seconds")
        for p in proposals:
            con.execute("UPDATE minimax_distillations SET draft=?, words=?, updated_at=? "
                        "WHERE article_id=?",
                        (p["kept"], len(p["kept"].split()), now, p["article_id"]))
            con.execute(
                "INSERT INTO cleanup_audit (run_id, article_id, language, field, model,"
                " prompt_version, status, changed, original_sha256, cleaned_sha256,"
                " original_text, cleaned_text, removed_text, usage_json, error, created_at)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (run_id, p["article_id"], "en", FIELD, MODEL, PROMPT_VERSION, "accepted", 1,
                 _sha(p["original"]), _sha(p["kept"]), p["original"], p["kept"],
                 "\n".join(p["removed_lines"]), None, None, now))
        con.commit()
    except Exception:
        con.rollback()
        raise
    finally:
        con.close()
    print(f"\napplied {len(proposals)} removals in run {run_id}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
