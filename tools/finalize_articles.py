#!/usr/bin/env python3
"""Apply the article invariants to every article in the content checkout.

Article text lives in the companion ``pocketwiki-content`` repository; see
``tools/content_paths.py`` for how that checkout is found. This tool walks it
and reports, or applies, the three invariants enforced by
``tools/article_text.py``:

- no trailing generator commentary (edit logs, word-count self-assessments),
- exactly one ``h1`` title, with later ``h1`` headings demoted to ``h2``,
- a CC BY-SA 4.0 attribution footer naming the source article.

It is idempotent: running it twice changes nothing the second time.

    # Report what would change (default).
    python3 tools/finalize_articles.py

    # Rewrite the article files in place, and write a JSON report.
    python3 tools/finalize_articles.py --apply --report finalize-report.json

    # CI: exit non-zero if any article is not finalized.
    python3 tools/finalize_articles.py --check
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from article_text import finalize_article  # noqa: E402
from content_paths import ARTICLES  # noqa: E402


def finalize_tree(root: Path, apply: bool = False) -> tuple[list[dict], list[str]]:
    """Finalize every ``.md`` file under ``root``.

    Returns the per-file change records and the paths that could not be
    finalized.
    """
    changes: list[dict] = []
    errors: list[str] = []
    for path in sorted(root.rglob("*.md")):
        original = path.read_text(encoding="utf-8")
        try:
            final, report = finalize_article(original)
        except ValueError as exc:
            errors.append(f"{path}: {exc}")
            continue
        if final == original:
            continue
        changes.append({
            "path": str(path),
            "title": report["title"],
            "removed_lines": report["removed_lines"],
            "demoted_headings": report["demoted_headings"],
            "footer_added": report["footer_added"],
        })
        if apply:
            path.write_text(final, encoding="utf-8")
    return changes, errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--content", type=Path, default=ARTICLES,
                        help="article root to process (default: the content checkout)")
    parser.add_argument("--apply", action="store_true",
                        help="rewrite the files; without this, only report")
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero when any article is not finalized")
    parser.add_argument("--report", type=Path,
                        help="write the per-file change records as JSON here")
    parser.add_argument("--quiet", action="store_true",
                        help="print only the summary")
    args = parser.parse_args(argv)

    if not args.content.is_dir():
        print(f"article root not found: {args.content}", file=sys.stderr)
        return 2

    changes, errors = finalize_tree(args.content, apply=args.apply)

    for error in errors:
        print(f"error: {error}", file=sys.stderr)

    if not args.quiet:
        for change in changes:
            rel = Path(change["path"]).name
            notes = []
            if change["removed_lines"]:
                notes.append(f"{len(change['removed_lines'])} commentary lines")
            if change["demoted_headings"]:
                notes.append(f"{len(change['demoted_headings'])} headings")
            if change["footer_added"]:
                notes.append("footer")
            print(f"{rel}: {', '.join(notes)}")

    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(changes, indent=2, ensure_ascii=False) + "\n",
                               encoding="utf-8")

    verb = "updated" if args.apply else "need changes"
    print(f"{len(changes)} of {len(list(args.content.rglob('*.md')))} articles {verb}; "
          f"{len(errors)} with errors")

    if args.check and changes:
        return 1
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
