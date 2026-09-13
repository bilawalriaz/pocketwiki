"""Guard behaviour of the quote-anchored remover.

This tool deletes the block containing a model's verified quote, so its refusals
matter more than its removals. Each case below comes from the corpus: the
heading refusal is what stopped "Évariste Galois constructed GL(ν, p) ..." and
"Major findings include the first gamma-ray-only pulsar ..." from being deleted
as if they were notes.
"""

import pytest

from article_text import MAX_REMOVAL_FRACTION, MIN_REMOVAL_LIMIT
from apply_audit_removals import propose


def test_removes_the_block_containing_the_quote():
    text = (
        "# Topic\n\nReal article prose about the subject.\n\n"
        "**Key edits:**\n- Fixed a typo\n- Removed filler\n"
    )
    result = propose(text, ["**Key edits:**", "- Fixed a typo"])
    assert result is not None and "refused" not in result
    assert "**Key edits:**" not in result["kept"]
    assert "Fixed a typo" not in result["kept"]
    assert "Real article prose about the subject." in result["kept"]


def test_heading_block_falls_back_to_the_quoted_line():
    """A note directly above the title must not take the title with it.

    Mirrors the Gap metric and Transfer learning drafts, whose notes open the
    file immediately above their own heading.
    """
    text = (
        "This should be ~900-950 words and well under 12k bytes.\n"
        "# Gap metric\n\nReal article prose.\n"
    )
    result = propose(text, ["This should be ~900-950 words and well under 12k bytes."])
    assert result is not None and "refused" not in result
    assert "# Gap metric" in result["kept"]
    assert "well under 12k bytes" not in result["kept"]


def test_a_quote_that_is_a_heading_is_ignored():
    """The model sometimes flags the article's own title; that is not content."""
    assert propose("# Real title\n\nProse.\n", ["# Real title"]) is None


def test_merges_several_quotes_into_one_block():
    text = (
        "# Topic\n\nReal prose.\n\n"
        "**Changes:**\n- A one\n- B two\n- C three\n"
    )
    result = propose(text, ["**Changes:**", "- B two"])
    assert "refused" not in result
    for fragment in ("**Changes:**", "- A one", "- B two", "- C three"):
        assert fragment not in result["kept"]


def test_refuses_a_removal_larger_than_the_cap():
    """A quote inside a long unbroken paragraph would delete the paragraph."""
    paragraph = "Real article prose about the subject. " * 60
    text = f"# Topic\n\n{paragraph}\n"
    result = propose(text, ["Real article prose about the subject. Real article prose"])
    assert result is not None
    assert "refused" in result
    assert MAX_REMOVAL_FRACTION < 1 and MIN_REMOVAL_LIMIT > 0


def test_ignores_a_quote_that_is_not_in_the_text():
    text = "# Topic\n\nReal prose.\n"
    assert propose(text, ["text that is nowhere in the draft"]) is None
