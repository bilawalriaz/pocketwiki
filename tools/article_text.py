#!/usr/bin/env python3
"""Canonical article text handling for the PocketWiki content corpus.

Articles in the ``pocketwiki-content`` repository are generated, mostly by
``tools/export_distilled.py`` and ``tools/build_db_packs.py`` from a local wiki
distill database. Every generated file must satisfy three properties:

1. exactly one leading ``# Title``, which is the source Wikipedia article title;
2. no self-assessment commentary from the generating model -- no edit logs, no
   word-count checks, no notes about the writing process;
3. a CC BY-SA 4.0 attribution footer naming the creator and linking the source.

:func:`finalize_article` enforces all three. The generators call it before
writing, and ``tools/finalize_articles.py`` applies it to articles that are
already checked in, so the two paths cannot drift.

The self-assessment rules are deliberately narrow. ``## What Changed for
Society`` is a legitimate section heading in an article about social change, so
only a *bolded* "Changes made"-style header counts as generator commentary.
"""

from __future__ import annotations

import re
import urllib.parse

LICENSE_URL = "https://creativecommons.org/licenses/by-sa/4.0/"
SOURCE_BASE = "https://en.wikipedia.org/wiki/"

# A trailing block the generating model wrote about its own edits. The bold
# markers and the "made/changed" phrasing are what separate this from a real
# section heading such as "## What changed and why".
SELF_REPORT_HEADER = re.compile(
    r"^\s*\*\*\s*("
    r"changes made|summary of changes|key changes|edit log|edits made|"
    r"revisions? made|modifications made|notes on (?:the )?(?:draft|edits)"
    r")\b[^*]*\*\*",
    re.I,
)

# Sentences that name the artifact's own constraints. Encyclopedic prose does
# not talk about its own word budget, so these are safe signals, but they are
# only trusted near the end of a file: "No word count is fixed" is real content
# in an article about short stories, and it sits in the body, not the tail.
#
# Only *framed* mentions of meta-commentary count. An earlier revision matched a
# bare "meta-commentary", which stripped a legitimate bullet about Las Meninas
# ("a meta-commentary on the act of painting") -- the painting is about
# representation, so the phrase is subject matter. "no meta-conclusions" and
# "rather than a meta-summary" are the generator judging its own output, so the
# framing words are required.
#
# The other phrasings here were each observed in this corpus, found by
# tools/audit_meta_commentary.py in articles these rules had judged clean.
SELF_ASSESSMENT = re.compile(
    r"("
    r"(?:no|not on|rather than|instead of|avoids?|avoided|without|forbidden|"
    r"prohibited)\s+(?:a\s+|any\s+)?meta-\w+|"
    r"prohibited material|"
    r"note on edits?|notes? on (?:the )?(?:draft|edits?|editing)|"
    r"the lesson (?:was|is) (?:already )?within limits|"
    r"the current lesson is approximately|"
    # A count label only counts as self-assessment when a number follows, so
    # "No word count is fixed" in an article about short stories is left alone.
    r"(?:word count|length check|word check)[^.\n]{0,24}?\d|"
    r"words?\)?\s*(?:target|budget|limit)|"
    r"well under (?:the )?(?:target|\d)|"
    r"well within (?:the \d|the (?:word|length) (?:target|budget|limit))|"
    r"hard[ _]?max"
    r")",
    re.I,
)

# Matches a footer this module wrote, so finalizing twice is a no-op.
FOOTER_RE = re.compile(r"^Source: adapted from .*CC BY-SA 4\.0.*$", re.M)

# Third-person edit narration: "The edit trims filler (...)", "This revision
# merges ...". Found by tools/audit_meta_commentary.py on a sample of articles
# these rules had judged clean, which is the loop that tool exists to drive:
# audit a sample, then promote what it finds into a narrow rule here.
#
# Kept to the same tail window as SELF_ASSESSMENT. In an article about film
# editing, "the edit removes the subplot" is real content, and such a sentence
# belongs in the body; generator notes accumulate at the end of the file.
EDIT_NARRATION = re.compile(
    r"\b(?:the|this)\s+(?:edit|revision|rewrite)\b"
    r"[^.]{0,200}?\b(?:trims?|trimmed|removes?|removed|replaces?|replaced|merges?|"
    r"merged|softens?|softened|preserves?|preserved|tightens?|tightened|"
    r"condenses?|condensed|rewrit(?:es|ten)|adds?|added|cuts?|drops?|dropped|"
    r"fixes|fixed|clarifies|clarified|expands?|expanded|keeps?|kept|reduces?|reduced)\b",
    re.I,
)

# How many trailing non-blank lines may count as "the tail" for the
# self-assessment rules. Small on purpose.
TAIL_LINES = 6


def h1_title(text: str) -> str | None:
    """Return the article's ``h1`` title, or ``None`` when there is not exactly one.

    Fenced code blocks are skipped, so a ``#`` comment in a shell or Python
    snippet is not mistaken for a title and cannot make an article look as
    though it has several.
    """
    titles: list[str] = []
    in_fence = False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = re.match(r"^#\s+(.+?)\s*$", line)
        if match:
            titles.append(match.group(1).strip())
    return titles[0] if len(titles) == 1 else None


def source_url(title: str) -> str:
    """The English Wikipedia URL for an article title."""
    return SOURCE_BASE + urllib.parse.quote(title.replace(" ", "_"))


def attribution_footer(title: str, source: str | None = None) -> str:
    """The CC BY-SA 4.0 footer: creator, license URI, and source URI.

    ``source`` overrides the URL derived from the title, for callers that
    already hold the canonical article URL.
    """
    return (
        f'\nSource: adapted from "{title}" on English Wikipedia, whose text is '
        f"written by Wikipedia contributors, under CC BY-SA 4.0 "
        f"({LICENSE_URL}): {source or source_url(title)}\n"
    )


def _contamination_start(lines: list[str]) -> int | None:
    """Index of the first line of generator commentary, or ``None``."""
    for i, line in enumerate(lines):
        if SELF_REPORT_HEADER.match(line):
            return i
    tail = [i for i, line in enumerate(lines) if line.strip()][-TAIL_LINES:]
    for i in reversed(tail):
        if SELF_ASSESSMENT.search(lines[i]) or EDIT_NARRATION.search(lines[i]):
            return i
    return None


def strip_meta_commentary(text: str) -> tuple[str, list[str]]:
    """Remove trailing generator commentary.

    Returns the cleaned text and the non-blank lines that were removed, so
    callers can report or review exactly what changed.
    """
    lines = text.splitlines()
    start = _contamination_start(lines)
    if start is None:
        return text, []
    # Drop the horizontal rule or blank lines that introduced the block.
    while start > 0 and lines[start - 1].strip() in ("", "---", "***", "___"):
        start -= 1
    removed = [line for line in lines[start:] if line.strip()]
    kept = "\n".join(lines[:start]).rstrip() + "\n"
    return kept, removed


def finalize_article(text: str, source: str | None = None) -> tuple[str, dict]:
    """Apply all invariants. Idempotent.

    Returns the final text and a report describing what changed.
    """
    cleaned, removed = strip_meta_commentary(text)
    cleaned, headings = normalize_headings(cleaned)
    title = h1_title(cleaned)
    if title is None:
        raise ValueError("article must have exactly one h1 title")

    without_footer = FOOTER_RE.sub("", cleaned).rstrip() + "\n"
    final = without_footer + attribution_footer(title, source)
    return final, {
        "title": title,
        "removed_lines": removed,
        "demoted_headings": headings,
        "footer_added": not FOOTER_RE.search(cleaned),
    }


def normalize_headings(text: str) -> tuple[str, list[str]]:
    """Keep the first ``h1`` as the article title and demote later ones to ``h2``.

    Generated articles sometimes emit section headings as ``h1``, which renders
    them as top-level headings on the device. An ``h1`` that merely repeats the
    title is dropped. Heading-looking lines inside fenced code blocks are left
    alone, because ``#`` starts a comment in shell and Python snippets.

    Returns the text and the headings that were demoted or dropped.
    """
    lines = text.splitlines()
    out: list[str] = []
    changed: list[str] = []
    title: str | None = None
    in_fence = False
    for line in lines:
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            out.append(line)
            continue
        if not in_fence and line.startswith("# "):
            heading = line[2:].strip()
            if title is None:
                title = heading
                out.append(line)
                continue
            if heading.casefold() == title.casefold():
                changed.append(heading)
                continue
            out.append("## " + heading)
            changed.append(heading)
            continue
        out.append(line)
    if not changed:
        return text, []
    # Dropping a heading can leave a run of blank lines behind.
    collapsed: list[str] = []
    for line in out:
        if line.strip() == "" and collapsed and collapsed[-1].strip() == "":
            continue
        collapsed.append(line)
    return "\n".join(collapsed).rstrip() + "\n", changed


def is_finalized(text: str) -> bool:
    """True when the text already satisfies every invariant."""
    try:
        return finalize_article(text)[0] == text
    except ValueError:
        return False
