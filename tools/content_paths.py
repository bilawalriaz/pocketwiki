#!/usr/bin/env python3
"""Locate the article corpus that this code repository builds packs from.

Article text is adapted from Wikipedia and is licensed CC BY-SA 4.0, so it is
not distributed in this Apache-2.0 repository. It lives in the companion
``pocketwiki-content`` repository alongside ``packs/catalog-source.json``, the
pack manifest that lists the articles in each pack.

Resolution order:

1. ``POCKETWIKI_CONTENT_DIR`` — an explicit path to the content checkout.
2. ``../pocketwiki-content`` — the content repository checked out next to this
   one, which is what a normal two-repo clone gives you.

Nothing here touches the network. Firmware and pack builds read whatever
checkout these paths resolve to, so offline builds keep working.
"""

from __future__ import annotations

import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
ENV_VAR = "POCKETWIKI_CONTENT_DIR"

CONTENT = Path(
    os.environ[ENV_VAR]
).expanduser().resolve() if os.environ.get(ENV_VAR) else REPO_ROOT.parent / "pocketwiki-content"
ARTICLES = CONTENT / "articles"
CATALOG = CONTENT / "packs" / "catalog-source.json"

# The only article-shaped data that stays in this repository: two short
# articles that exercise the packer and decoders. Tests that only need a small
# corpus must use this, so the host suite runs without a content checkout.
SAMPLE = REPO_ROOT / "tests" / "fixtures" / "articles" / "sample"


def require(path: Path) -> Path:
    """Return ``path``, or explain how to obtain the content repository."""
    if path.exists():
        return path
    raise FileNotFoundError(
        f"{path} is missing.\n"
        f"The article corpus lives in the separate pocketwiki-content "
        f"repository. Clone it beside this one:\n"
        f"  git clone <content-repo-url> {CONTENT}\n"
        f"or point {ENV_VAR} at an existing checkout."
    )


def available() -> bool:
    """True when a content checkout with both the corpus and manifest is present."""
    return ARTICLES.is_dir() and CATALOG.is_file()
