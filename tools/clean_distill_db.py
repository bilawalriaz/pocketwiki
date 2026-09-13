#!/usr/bin/env python3
"""Strip generator commentary from the wiki-distill database, in place.

The database at ``~/wiki-distill/educational-source.db`` is the source the pack
generators read (``minimax_distillations.draft``). Its drafts were produced by a
writing agent that could not write files, so many of them end with the agent
explaining itself: requests for write permission, edit summaries, word and byte
counts against the limits, and in two cases a note about a prompt injection.

``tools/article_text.py`` holds the rules, because the same rules also guard the
exporter path. This tool applies them to the database and leaves an audit trail
in ``cleanup_audit``, matching what the earlier ``meta-cleanup-*`` passes did.

Safety, because this database is the only copy:

* a backup is written next to it before anything is modified;
* every change is applied inside one transaction;
* a change that would remove more than ``--max-removal-fraction`` of a draft, or
  leave it implausibly short, is skipped and reported rather than applied;
* the pass is idempotent, and ``--verify`` re-scans to prove it.

    # Report what would change. Nothing is written.
    python3 tools/clean_distill_db.py

    # Back up, then apply, writing a JSON report.
    python3 tools/clean_distill_db.py --apply --report build/db-cleanup.json

    # Prove it is clean, and that a second pass would change nothing.
    python3 tools/clean_distill_db.py --verify
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sqlite3
import sys
import unicodedata
import uuid
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from article_text import strip_meta_commentary  # noqa: E402

DEFAULT_DB = Path(os.environ.get(
    "POCKETWIKI_DISTILL_DB", Path.home() / "wiki-distill" / "educational-source.db"))

FIELD = "minimax_distillations.draft"
MODEL = "deterministic"
PROMPT_VERSION = "meta-cleanup-deterministic-v3"
MIN_KEPT_CHARS = 200


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _words(text: str) -> int:
    """Whitespace word count, which is how the table's words column is derived."""
    return len(text.split())


def _norm(text: str) -> str:
    return unicodedata.normalize("NFKC", text or "")


def plan(con: sqlite3.Connection, max_fraction: float, only_ids: set[int] | None,
         limit: int | None) -> tuple[list[dict], list[dict]]:
    """Work out what would change. Returns (changes, skipped)."""
    query = "SELECT article_id, title, draft, words FROM minimax_distillations"
    params: list[object] = []
    if only_ids:
        query += f" WHERE article_id IN ({','.join('?' * len(only_ids))})"
        params = sorted(only_ids)
    query += " ORDER BY article_id"

    changes: list[dict] = []
    skipped: list[dict] = []
    for aid, title, draft, words in con.execute(query, params):
        original = draft or ""
        if not original.strip():
            continue
        kept, removed = strip_meta_commentary(original)
        if not removed:
            continue
        record = {
            "article_id": aid,
            "title": title,
            "original": original,
            "cleaned": kept,
            "removed": "\n".join(removed),
            "removed_lines": removed,
            "removed_chars": len(original) - len(kept),
            "fraction": (len(original) - len(kept)) / max(1, len(original)),
            "words_before": _words(original),
            "words_after": _words(kept),
        }
        if record["fraction"] > max_fraction:
            record["reason"] = f"removal is {record['fraction']:.0%} of the draft"
            skipped.append(record)
        elif len(kept) < MIN_KEPT_CHARS:
            record["reason"] = f"would leave {len(kept)} characters"
            skipped.append(record)
        else:
            changes.append(record)
        if limit and len(changes) >= limit:
            break
    return changes, skipped


def already_done(con: sqlite3.Connection) -> set[int]:
    """Article ids this prompt version has already accepted a cleanup for."""
    return {
        row[0] for row in con.execute(
            "SELECT article_id FROM cleanup_audit WHERE field=? AND prompt_version=? "
            "AND status='accepted' AND changed=1", (FIELD, PROMPT_VERSION))
    }


def apply_changes(db: Path, changes: list[dict], run_id: str) -> None:
    """Write the changes and their audit rows in one transaction."""
    con = sqlite3.connect(db)
    try:
        con.execute("BEGIN")
        now = datetime.now(timezone.utc).isoformat(timespec="seconds")
        for change in changes:
            con.execute(
                "UPDATE minimax_distillations SET draft=?, words=?, updated_at=? "
                "WHERE article_id=?",
                (change["cleaned"], change["words_after"], now, change["article_id"]))
            con.execute(
                "INSERT INTO cleanup_audit (run_id, article_id, language, field, model,"
                " prompt_version, status, changed, original_sha256, cleaned_sha256,"
                " original_text, cleaned_text, removed_text, usage_json, error, created_at)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (run_id, change["article_id"], "en", FIELD, MODEL, PROMPT_VERSION,
                 "accepted", 1, _sha(change["original"]), _sha(change["cleaned"]),
                 change["original"], change["cleaned"], change["removed"], None, None, now))
        con.commit()
    except Exception:
        con.rollback()
        raise
    finally:
        con.close()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", type=Path, default=DEFAULT_DB)
    parser.add_argument("--apply", action="store_true",
                        help="write the changes; without this, only report")
    parser.add_argument("--no-backup", action="store_true",
                        help="skip the backup copy (not recommended)")
    parser.add_argument("--max-removal-fraction", type=float, default=0.35,
                        help="skip drafts where the removal would exceed this share")
    parser.add_argument("--ids", help="comma-separated article ids to limit the pass to")
    parser.add_argument("--limit", type=int, help="stop after this many changes")
    parser.add_argument("--report", type=Path, help="write the full change list as JSON")
    parser.add_argument("--verify", action="store_true",
                        help="report what a further pass would change, then exit")
    args = parser.parse_args(argv)

    if not args.db.is_file():
        print(f"database not found: {args.db}", file=sys.stderr)
        return 2

    only_ids = {int(x) for x in args.ids.split(",")} if args.ids else None
    con = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    try:
        done = already_done(con)
        changes, skipped = plan(con, args.max_removal_fraction, only_ids, args.limit)
    finally:
        con.close()

    changes = [c for c in changes if c["article_id"] not in done]
    total_chars = sum(c["removed_chars"] for c in changes)
    print(f"database: {args.db}")
    print(f"drafts to clean: {len(changes)}   characters removed: {total_chars:,}   "
          f"skipped by guards: {len(skipped)}")
    for item in sorted(changes, key=lambda c: -c["removed_chars"])[:10]:
        print(f"  -{item['removed_chars']:5d} ({item['fraction']:4.0%})  "
              f"id={item['article_id']:6d} {item['title'][:38]:40s} "
              f"{item['removed_lines'][0][:60]!r}")
    for item in skipped:
        print(f"  SKIPPED id={item['article_id']}: {item['reason']}")

    if args.verify:
        return 1 if changes else 0

    if not changes:
        print("nothing to do")
        return 0

    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(
            [{k: v for k, v in c.items() if k not in ("original", "cleaned")}
             for c in changes], indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"wrote {args.report}")

    if not args.apply:
        print("\ndry run: nothing written. Re-run with --apply.")
        return 0

    if not args.no_backup:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        backup = args.db.with_name(f"{args.db.name}.bak-{stamp}")
        print(f"backing up to {backup} ...", flush=True)
        shutil.copy2(args.db, backup)
        print(f"backup written ({backup.stat().st_size / 1e9:.2f} GB)")

    run_id = str(uuid.uuid4())
    apply_changes(args.db, changes, run_id)
    print(f"applied {len(changes)} changes in run {run_id}")

    con = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    try:
        remaining, _ = plan(con, args.max_removal_fraction, only_ids, None)
        remaining = [c for c in remaining if c["article_id"] not in already_done(con)]
    finally:
        con.close()
    print(f"re-scan: {len(remaining)} drafts would still change")
    return 1 if remaining else 0


if __name__ == "__main__":
    raise SystemExit(main())
