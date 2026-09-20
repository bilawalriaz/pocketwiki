#!/usr/bin/env python3
"""Locate the article corpus that this repository builds packs from.

Most of the article text is adapted from English Wikipedia and is licensed
CC BY-SA 4.0, so it lives in the companion ``pocketwiki-content`` repository
rather than here. That checkout holds ``articles/`` and
``packs/catalog-source.json``, the manifest naming the articles in each pack.

Resolution order:

1. ``POCKETWIKI_CONTENT_DIR``, an explicit path to the content checkout.
2. ``../pocketwiki-content``, the content repository cloned next to this one,
   which is what a normal two-repo clone gives you.

One pack is the exception. ``pocketwiki`` is the device's own guide, written for
this project rather than adapted from Wikipedia, and it ships with the firmware,
so its articles stay in ``articles/pocketwiki`` here. Everything in
``catalog-source.json`` resolves through :func:`article_dir`, which knows about
that one directory.

Nothing here touches the network. Pack and firmware builds read whatever
checkout these paths resolve to, so offline builds keep working.
"""

from __future__ import annotations

import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
ENV_VAR = "POCKETWIKI_CONTENT_DIR"

CONTENT = (
    Path(os.environ[ENV_VAR]).expanduser().resolve()
    if os.environ.get(ENV_VAR)
    else REPO_ROOT.parent / "pocketwiki-content"
)
ARTICLES = CONTENT / "articles"
CATALOG = CONTENT / "packs" / "catalog-source.json"
LABELS = CONTENT / "packs" / "cluster-labels.json"
SELECTION = CONTENT / "packs" / "catalogue-selection.json"
SHELF_MANIFEST = ARTICLES / "cluster-packs" / "manifest.json"
CATALOGUE_BROWSER = CONTENT / "packs.html"

# The built-in guide, and the only article-shaped data that stays in this
# repository.
BUILT_IN = "pocketwiki"
GUIDE = REPO_ROOT / "articles" / BUILT_IN

# Two short articles that exercise the packer and the decoders. Tests that only
# need a small corpus use these, so the host suite runs without a content
# checkout.
SAMPLE = REPO_ROOT / "tests" / "fixtures" / "articles" / "sample"


def article_dir(name: str) -> Path:
    """Resolve a catalogue ``dir`` value to the directory that holds it."""
    if name == BUILT_IN or name.startswith(BUILT_IN + "/"):
        return REPO_ROOT / "articles" / name
    return ARTICLES / name


def require(path: Path) -> Path:
    """Return ``path``, or explain how to obtain the content repository."""
    if path.exists():
        return path
    raise FileNotFoundError(
        f"{path} is missing.\n"
        f"Most article text lives in the separate pocketwiki-content "
        f"repository. Clone it beside this one:\n"
        f"  git clone https://github.com/bilawalriaz/pocketwiki-content.git {CONTENT}\n"
        f"or point {ENV_VAR} at an existing checkout."
    )


def available() -> bool:
    """True when a content checkout with both the corpus and manifest is present."""
    return ARTICLES.is_dir() and CATALOG.is_file()
