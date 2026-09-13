"""The glued-heading splitter, which must never touch legitimate CamelCase.

This heuristic inserts a newline, so a wrong split cannot lose text, but it can
still mangle a word: "DevOps" becoming "Dev Ops", or the gene name "FoxI1e"
becoming "Fox" / "I1e". Those cases are pinned below, from the corpus.
"""

from fix_glued_headings import find_split


def test_splits_a_heading_glued_to_its_first_sentence():
    line = "## MaterialsThe pressure vessel is usually steel, or historically wrought iron."
    at = find_split(line)
    assert at is not None
    assert line[:at].strip() == "## Materials"
    assert line[at:].startswith("The pressure vessel is usually steel")


def test_splits_when_the_heading_ends_in_a_short_word():
    """'What it was' plus 'A sol is one Martian day ...' (Spirit rover)."""
    line = "## What it wasA sol is one Martian day, about 24 hours and 37 minutes. Spirit was a rover."
    at = find_split(line)
    assert at is not None
    assert line[:at].strip() == "## What it was"
    assert line[at:].startswith("A sol is one Martian day")


def test_leaves_legitimate_camel_case_headings_alone():
    for line in (
        "## Relationship to DevOps and continuous deployment",
        "## Chromosomes in evolution: McClintock and Dobzhansky",
        "## FoxI1e: epidermal fate and a repressor of the other layers",
        "## The role of GitHub in open source",
    ):
        assert find_split(line) is None, line


def test_leaves_ordinary_long_headings_alone():
    for line in (
        "## Decline and the Post-Classical Era (c. 500–1500)",
        "## The modern era: openness, collapse, and rebuilding",
        "## A continent built on old rock and young mountains",
    ):
        assert find_split(line) is None, line


def test_ignores_non_headings():
    assert find_split("MaterialsThe pressure vessel is usually steel, or historically wrought iron.") is None
