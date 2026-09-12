#!/usr/bin/env python3
"""Fetch the curated 100-article PocketWiki starter pack from Wikipedia.

The checked-in output retains a source link and CC BY-SA attribution on every
article. Re-running this script is explicit; ordinary builds stay offline and
deterministic.
"""

from __future__ import annotations

import json
import pathlib
import re
import time
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
from content_paths import ARTICLES
OUT = ARTICLES / "starter"

TITLES = [
    "Agriculture", "Algebra", "Algorithm", "Antibiotic", "Architecture",
    "Artificial intelligence", "Astronomy", "Atom", "Bacteria", "Battery (electricity)",
    "Biodiversity", "Blockchain", "Boolean algebra", "Calculus", "Cell (biology)",
    "Chemical element", "Climate change", "Cloud computing", "Computer", "Computer network",
    "Constitution", "Cryptography", "Database", "Democracy", "DNA",
    "Earth", "Ecology", "Electricity", "Electromagnetic spectrum", "Energy",
    "Engineering", "Evolution", "Financial literacy", "First aid", "Food preservation",
    "French Revolution", "Geography", "Geology", "Global Positioning System", "Government",
    "Great Depression", "Greenhouse effect", "Human body", "Human rights", "Industrial Revolution",
    "Inflation", "Internet", "Machine learning", "Magnetism", "Mathematics",
    "Memory", "Microcontroller", "Microscope", "Moon", "Nutrition",
    "Operating system", "Photosynthesis", "Physics", "Plate tectonics", "Probability",
    "Programming language", "Public health", "Quantum mechanics", "Radio", "Recycling",
    "Renewable energy", "Roman Empire", "Scientific method", "Semiconductor", "Simple machine",
    "Solar System", "Solar power", "Statistics", "Steam engine", "Supply and demand",
    "Sustainable development", "Tax", "Telephone", "Theory of relativity", "Thermodynamics",
    "Time", "Transistor", "United Nations", "Vaccination", "Water",
    "Water cycle", "Weather", "Web browser", "Wi-Fi", "World War I",
    "World War II", "Writing", "World Wide Web", "Acid–base reaction", "Computer security",
    "Digital literacy", "Earthquake", "Ecosystem", "Human evolution", "Logic",
]


def slug(value: str) -> str:
    value = value.casefold().replace("–", "-")
    return re.sub(r"[^a-z0-9]+", "-", value).strip("-")


def fetch(title: str) -> dict:
    encoded = urllib.parse.quote(title.replace(" ", "_"), safe="()_")
    url = f"https://en.wikipedia.org/api/rest_v1/page/summary/{encoded}"
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "PocketWiki/1.0 (https://github.com/bilawalriaz/pocketwiki-oss)"},
    )
    with urllib.request.urlopen(request, timeout=20) as response:
        return json.load(response)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    failures: list[str] = []
    for index, title in enumerate(TITLES, 1):
        try:
            data = fetch(title)
            display = data.get("title") or title
            description = data.get("description", "")
            extract = data.get("extract", "").strip()
            source = data.get("content_urls", {}).get("desktop", {}).get("page", "")
            if not extract or not source:
                raise ValueError("summary or source missing")
            body = [f"# {display}", ""]
            if description:
                body.extend([f"*{description[0].upper() + description[1:]}.*", ""])
            body.extend([
                extract,
                "",
                "## Source and licence",
                "",
                f"Adapted from [{display}]({source}) on Wikipedia. Text is available under the "
                "[Creative Commons Attribution-ShareAlike License](https://creativecommons.org/licenses/by-sa/4.0/).",
                "",
            ])
            (OUT / f"{index:03d}-{slug(display)}.md").write_text("\n".join(body), encoding="utf-8")
            print(f"[{index:03d}/100] {display}")
            time.sleep(0.04)
        except Exception as exc:  # keep the complete failure list actionable
            failures.append(f"{title}: {exc}")
    if failures:
        raise SystemExit("Failed articles:\n" + "\n".join(failures))


if __name__ == "__main__":
    main()
