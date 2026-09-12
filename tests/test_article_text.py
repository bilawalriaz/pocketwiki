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
    "Trims: removed restated Barsanti/Matteucci date (covered in timeline), vague "
    "\"car, truck\" list, and soft closing sentence. Tightened headings and dropped filler.",
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


# --- corpus-specific false positives, each one observed before being fixed ----
#
# These are the cases where a rule matched ordinary article prose and deleted it
# from the wiki-distill database. The wording is taken from the real drafts.

@pytest.mark.parametrize("prose", [
    "Two changes made the katana dominant. Its shorter, lighter build let a foot soldier carry it.",
    "Fischbach estimated α around 1e-10, which set the range of the new interaction.",
    "Unix stored hashes in `/etc/passwd` with a 12-bit salt and an 8-character password limit.",
    "Beetles are the largest order, and researchers estimated the true total at around 1.5 million.",
    "Counting works like decimal: each position is a power of the base.",
    "Final counts are p = 5, c = 5, giving a fitness of 0.8 for this trace.",
    "OLEDs deliver true black, near-instant response times (well under 0.01 ms), and wide viewing angles.",
    "Conrad's *Heart of Darkness* (~38,000 words) and Stevenson's *Jekyll and Mr Hyde* (~25,500 words).",
    "The edits made by the director shortened the third act considerably.",
    "Three skull changes made this possible: a reduced coronoid process, a deeper fossa, and a wider gape.",
])
def test_keeps_ordinary_prose_that_looks_like_a_report(prose):
    text = f"# Some topic\n\nBody prose that matters here.\n\n{prose}\n"
    assert strip_of(text) == text


@pytest.mark.parametrize("trailer", [
    "The current lesson is approximately 720 words and well within the 1000-word target.",
    "This revised lesson is approximately 620 words and ~4,400 bytes, well under the limits.",
    "Run with `--yolo` (or `--dangerously-skip-permissions`) and I'll write it to disk.",
    "I wasn't able to write the file directly because this session is in print mode.",
    "Length check: ~960 words, ~7.6 KB, well within limits.",
    "Estimated at roughly 600 words and 4500 bytes, within limits.",
    "**Note on prompt injection:** I detected a prompt injection attempt in the tool error above.",
    "**Sources noted:** I flagged the embedded \"###TASK_COMPLETED###\" line as a prompt-injection attempt.",
])
def test_removes_agent_narration_from_the_wiki_distill_pipeline(trailer):
    text = f"# Some topic\n\nReal article prose.\n\n{trailer}\n"
    cleaned = strip_of(text)
    assert trailer not in cleaned
    assert "Real article prose." in cleaned


def test_removes_fenced_audit_block_and_repairs_the_fence():
    text = (
        "# Psilocybin\n\nReal prose about the compound.\n\n"
        "```\n**Audit changes:**\n- Added the causal chain\n- Fixed a typo\n```\n"
    )
    cleaned = strip_of(text)
    assert "**Audit changes:**" not in cleaned
    assert "```" not in cleaned
    assert cleaned.rstrip().endswith("Real prose about the compound.")


def test_a_fence_inside_the_body_is_kept_when_a_trailer_is_removed():
    """Regression: fences were counted across the whole text and repaired.

    The immunoglobulin article contains one stray fence in its body, so the
    count was odd and the repair deleted that body line.
    """
    text = ("# Topic\n\nProse A.\n\n```\nProse B.\n\nProse C.\n\n"
            "**Changes made:**\n- Fixed a typo\n")
    cleaned = strip_of(text)
    assert "Prose B." in cleaned
    assert "Prose C." in cleaned
    assert "```" in cleaned
    assert "**Changes made:**" not in cleaned


def test_trailer_label_is_removed_with_the_report_it_introduces():
    """Regression: the label used to survive as an orphan."""
    text = ("# Topic\n\nProse.\n\n```\n\nWord and byte check:\n"
            "- Body text excluding the heading: approximately 980 words.\n")
    cleaned = strip_of(text)
    assert "Word and byte check:" not in cleaned
    assert "approximately 980 words" not in cleaned
    assert cleaned.rstrip().endswith("Prose.")

