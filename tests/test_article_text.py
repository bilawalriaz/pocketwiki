"""What ``finalize_article`` must change, and what it must leave alone.

The negative cases carry the weight here. Two revisions of the commentary rules
deleted real article content: matching a bare "meta-commentary" stripped a
bullet about *Las Meninas* (a painting about representation, so the phrase is
subject matter), and an early header rule treated "## What Changed for Society"
as generator commentary. Both are pinned below.
"""

import pytest

from article_text import attribution_footer, finalize_article, h1_title, strip_meta_commentary


def strip_of(text: str) -> str:
    return strip_meta_commentary(text)[0]


# --- must NOT be treated as generator commentary ------------------------------

def test_keeps_meta_commentary_as_subject_matter():
    """Las Meninas is about representation, so this is content, not a note."""
    text = (
        "# Las Meninas\n\n"
        "The painting resists a single reading.\n\n"
        "- **The Painting's Subject**: Is it a portrait of the Infanta, or a "
        "meta-commentary on the act of painting and representation itself?\n"
        "- **The Canvas**: Some argue it is *Las Meninas* itself, creating a "
        "self-referential loop.\n"
    )
    assert strip_of(text) == text


@pytest.mark.parametrize("heading", [
    "## What Changed for Society",
    "## What changed, region by region",
    "## What changed and why",
])
def test_keeps_what_changed_section_headings(heading):
    text = f"# Neolithic Revolution\n\nIntro text.\n\n{heading}\n\nBody text.\n"
    assert strip_of(text) == text


def test_keeps_prose_about_word_counts_in_the_body():
    """'No word count is fixed' is article content when it is not in the tail."""
    text = (
        "# Short story\n\n"
        "No word count is fixed. Typical short stories run 1,000 to 4,000 words.\n\n"
        "## History\n\n"
        + "More prose about the form. " * 60
        + "\n"
    )
    assert strip_of(text) == text


# --- must be removed ---------------------------------------------------------

@pytest.mark.parametrize("tail", [
    "The current lesson is approximately 720 words and well within the 1000-word target.",
    "About690 words, within the 1000-1100 target band, and ends on a substantive "
    "experimental fact rather than a meta-conclusion.",
    "Length check: ~960 words, ~7.6 KB, well within limits. Final paragraph ends on "
    "the fact that data is now routine, not on a meta-summary.",
    "Note on edits: removed the forbidden meta-conclusion paragraph, dropped a figure.",
    "The edit trims filler (\"sometimes awkward\"), merges two footnotes, and keeps the model.",
])
def test_removes_observed_self_assessment_lines(tail):
    text = f"# Some topic\n\nReal article prose.\n\n{tail}\n"
    cleaned = strip_of(text)
    assert tail not in cleaned
    assert "Real article prose." in cleaned


def test_removes_bolded_changes_made_block_to_end_of_file():
    text = (
        "# Some topic\n\nReal article prose.\n\n"
        "---\n\n"
        "**Changes made:**\n"
        "- Fixed a typo\n"
        "- Removed filler\n\n"
        "Word count is approximately 430 - well under the 950 target.\n"
    )
    cleaned = strip_of(text)
    assert "**Changes made:**" not in cleaned
    assert "Removed filler" not in cleaned
    assert cleaned.rstrip().endswith("Real article prose.")


# --- the finalized shape ------------------------------------------------------

def test_footer_is_added_with_source_and_licence():
    final, report = finalize_article("# Wi-Fi\n\nProse.\n")
    assert report["footer_added"] is True
    assert "CC BY-SA 4.0" in final
    assert "https://creativecommons.org/licenses/by-sa/4.0/" in final
    assert "https://en.wikipedia.org/wiki/Wi-Fi" in final
    assert "Wikipedia contributors" in final


def test_finalize_is_idempotent():
    once, _ = finalize_article("# Wi-Fi\n\nProse.\n\nThe edit trims filler.\n")
    twice, report = finalize_article(once)
    assert twice == once
    assert report["footer_added"] is False


def test_footer_is_not_duplicated_when_already_present():
    text = "# Wi-Fi\n\nProse.\n" + attribution_footer("Wi-Fi")
    final, _ = finalize_article(text)
    assert final.count("Source: adapted from") == 1


def test_later_h1_headings_are_demoted_and_duplicate_title_dropped():
    text = (
        "# Species\n\nIntro.\n\n"
        "# Species\n\n"
        "# Major species concepts\n\nBody.\n"
    )
    final, report = finalize_article(text)
    assert final.count("\n# ") == 0
    assert "## Major species concepts" in final
    assert final.count("# Species") == 1
    assert "Species" in report["demoted_headings"]


def test_heading_like_lines_in_code_fences_are_untouched():
    text = (
        "# Shell\n\nIntro.\n\n"
        "```sh\n# install the tool\nmake install\n```\n\n"
        "## Usage\n\nDone.\n"
    )
    final, _ = finalize_article(text)
    assert "# install the tool" in final


def test_missing_or_duplicated_h1_is_an_error():
    with pytest.raises(ValueError):
        finalize_article("No heading here.\n")
    assert h1_title("# One\n\n# Two\n") is None
