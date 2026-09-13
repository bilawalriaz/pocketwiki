#!/usr/bin/env python3
"""Split section headings that the generator glued to the sentence after them.

Some drafts contain lines such as:

    ## MaterialsThe pressure vessel is usually steel, or historically wrought iron.

The heading and the first sentence of its paragraph were emitted on one line, so
the device renders a long heading instead of a heading and a paragraph. This is
not the commentary problem: nothing is foreign to the article, and the audit
correctly reports these articles as clean.

Detection is deliberately conservative, because legitimate CamelCase exists in
this corpus (DevOps, McClintock, FoxI1e, GitHub):

* the line must be a heading longer than 50 characters;
* the split is at the first lowercase-to-uppercase boundary whose preceding word
  is at least three characters, so "Mc|Clintock" and "i|Phone" are skipped;
* the remainder must start uppercase, run at least 40 characters, contain a
  space within its first 25 characters, and its first token must contain no
  digits, which is what rejects "Fox|I1e: epidermal fate ...".

The change inserts one newline and deletes nothing, so a wrong split loses no
text; ``--verify`` re-checks that every change is exactly that insertion.

    python3 tools/fix_glued_headings.py            # report
    python3 tools/fix_glued_headings.py --apply    # write, with cleanup_audit rows
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sqlite3
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

DEFAULT_DB = Path(os.environ.get(
    "POCKETWIKI_DISTILL_DB", Path.home() / "wiki-distill" / "educational-source.db"))
FIELD = "minimax_distillations.draft"
MODEL = "deterministic"
PROMPT_VERSION = "heading-split-v1"

HEAD = re.compile(r"^(#{1,6})\s+(\S.*)$")
JAM = re.compile(r"[a-z][A-Z]")


def find_split(line: str) -> int | None:
    """Index in ``line`` at which to insert the newline, or ``None``."""
    match = HEAD.match(line)
    if not match or len(line) <= 50:
        return None
    body, offset = match.group(2), match.start(2)
    for jam in JAM.finditer(body):
        i = jam.start()
        if i < 2:
            continue
        prev = re.search(r"(\S+)$", body[:i + 1])
        if not prev or len(prev.group(1)) < 3:
            continue
        cut = i + 1
        head, rest = body[:cut].rstrip(), body[cut:]
        if not rest[:1].isupper() or len(rest) < 40:
            continue
        # A heading is short. Capping it below the 50-character line guard also
        # makes the pass idempotent: after splitting, the heading line can no
        # longer be long enough to match again.
        if len(head) > 47 or " " not in rest[:25]:
            continue
        first_token = re.match(r"\S+", rest).group(0)
        if any(ch.isdigit() for ch in first_token):
            continue
        return offset + cut
    return None


def plan(drafts: dict[int, str]) -> list[dict]:
    """One entry per draft, splitting its first still-glued heading.

    Repeated runs converge: each pass fixes one glued line per draft and a fixed
    line no longer matches, so 16 drafts with two glued headings need two passes
    rather than one entry hiding the other.
    """
    out = []
    for aid, text in drafts.items():
        lines = text.splitlines(keepends=True)
        for index, line in enumerate(lines):
            at = find_split(line.rstrip("\n"))
            if at is None:
                continue
            rest_of_line = line[at:]
            new_line = line[:at] + "\n" + rest_of_line
            fixed = "".join(lines[:index]) + new_line + "".join(lines[index + 1:])
            out.append({"article_id": aid, "original": text, "kept": fixed,
                        "before": line.strip()[:90], "after_header": line[:at].strip()[:60],
                        "after_sentence": rest_of_line.strip()[:60]})
            break
    return out


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", type=Path, default=DEFAULT_DB)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--verify", action="store_true",
                        help="assert every change is a single inserted newline")
    args = parser.parse_args(argv)

    con = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    drafts = dict(con.execute("SELECT article_id, COALESCE(draft,'') FROM minimax_distillations"))
    con.close()

    # No id-based skip: find_split is idempotent, so a fixed draft simply yields
    # nothing and repeated runs converge on drafts with several glued headings.
    changes = plan(drafts)
    print(f"glued headings to split: {len(changes)}")
    for c in changes[:12]:
        print(f"   id={c['article_id']:6d} {c['before']!r}")
        print(f"        -> {c['after_header']!r} + {c['after_sentence']!r}")

    if args.verify:
        # The real check: kept equals original with exactly one "\n" inserted.
        def is_insertion(original: str, new: str) -> bool:
            if len(new) != len(original) + 1:
                return False
            return any(new[i] == "\n" and new[:i] + new[i + 1:] == original
                       for i in range(len(new)))
        bad = [c["article_id"] for c in changes if not is_insertion(c["original"], c["kept"])]
        print(f"changes that are NOT a single inserted newline: {len(bad)} {bad[:5]}")
        return 1 if bad else 0

    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            "".join(f"{c['article_id']}\t{c['after_header']}\t{c['after_sentence']}\n" for c in changes),
            encoding="utf-8")

    if not args.apply or not changes:
        print("dry run: nothing written" if changes else "nothing to do")
        return 0

    con = sqlite3.connect(args.db)
    run_id = str(uuid.uuid4())
    try:
        con.execute("BEGIN")
        now = datetime.now(timezone.utc).isoformat(timespec="seconds")
        for c in changes:
            con.execute("UPDATE minimax_distillations SET draft=?, words=?, updated_at=? "
                        "WHERE article_id=?",
                        (c["kept"], len(c["kept"].split()), now, c["article_id"]))
            con.execute(
                "INSERT INTO cleanup_audit (run_id, article_id, language, field, model,"
                " prompt_version, status, changed, original_sha256, cleaned_sha256,"
                " original_text, cleaned_text, removed_text, usage_json, error, created_at)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (run_id, c["article_id"], "en", FIELD, MODEL, PROMPT_VERSION, "accepted", 1,
                 _sha(c["original"]), _sha(c["kept"]), c["original"], c["kept"],
                 "", None, None, now))
        con.commit()
    except Exception:
        con.rollback()
        raise
    finally:
        con.close()
    print(f"split {len(changes)} headings in run {run_id}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
