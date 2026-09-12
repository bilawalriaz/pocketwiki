#!/usr/bin/env python3
"""Create additional themed packs from the existing curated article shelves.

The source articles are copied locally and remain unchanged.  Catalogue
metadata follows the same shape as the established packs; this helper only
creates new content directories and appends the definitions.
"""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
from content_paths import ARTICLES, CATALOG

DB_PACKS = ARTICLES / "db-packs"


PACKS = [
    {
        "id": "space-and-the-cosmos",
        "name": "Space & the Cosmos",
        "description": "A guided tour through stars, planets, galaxies, gravity, light, and the questions that make astronomy fascinating.",
        "articles": [
            ("natural-sciences", "00023-astronomy.md"),
            ("natural-sciences", "00081-solar-system.md"),
            ("natural-sciences", "00094-universe.md"),
            ("natural-sciences", "00357-galaxy.md"),
            ("natural-sciences", "00377-gravity.md"),
            ("natural-sciences", "00499-light.md"),
            ("natural-sciences", "00544-mercury-planet.md"),
            ("natural-sciences", "00645-planet.md"),
            ("natural-sciences", "00675-quantum-mechanics.md"),
            ("natural-sciences", "00630-particle-physics.md"),
            ("natural-sciences", "00766-speed-of-light.md"),
            ("natural-sciences", "00772-star.md"),
            ("natural-sciences", "00779-subatomic-particle.md"),
            ("natural-sciences", "00806-theory-of-relativity.md"),
            ("natural-sciences", "00846-wave.md"),
            ("natural-sciences", "03085-history-of-astronomy.md"),
            ("natural-sciences", "05159-accelerating-expansion-of-the-universe.md"),
            ("natural-sciences", "05676-age-of-the-universe.md"),
            ("natural-sciences", "06394-ancient-greek-astronomy.md"),
            ("natural-sciences", "06667-asterism-astronomy.md"),
            ("natural-sciences", "13104-gravitational-wave-astronomy.md"),
            ("natural-sciences", "20283-quantum-information-science.md"),
        ],
    },
    {
        "id": "climate-and-the-living-planet",
        "name": "Climate & the Living Planet",
        "description": "A practical introduction to Earth systems, climate change, biodiversity, water, ecosystems, pollution, and resilience.",
        "articles": [
            ("earth-climate-environment", "00003-earth.md"),
            ("earth-climate-environment", "00022-atmosphere-of-earth.md"),
            ("earth-climate-environment", "00027-climate.md"),
            ("earth-climate-environment", "00078-ocean.md"),
            ("earth-climate-environment", "00106-water.md"),
            ("earth-climate-environment", "00194-biodiversity.md"),
            ("earth-climate-environment", "00218-carbon.md"),
            ("earth-climate-environment", "00219-carbon-dioxide.md"),
            ("earth-climate-environment", "00243-climate-change.md"),
            ("earth-climate-environment", "00297-drinking-water.md"),
            ("earth-climate-environment", "00310-ecosystem.md"),
            ("earth-climate-environment", "00655-pollution.md"),
            ("earth-climate-environment", "00848-weather.md"),
            ("earth-climate-environment", "00964-renewable-energy.md"),
            ("earth-climate-environment", "00976-solar-energy.md"),
            ("earth-climate-environment", "05322-climate-resilience.md"),
            ("earth-climate-environment", "06566-aquatic-ecosystem.md"),
            ("earth-climate-environment", "07139-biodiversity-loss.md"),
            ("earth-climate-environment", "08025-causes-of-climate-change.md"),
            ("earth-climate-environment", "08655-climate-change-mitigation.md"),
            ("earth-climate-environment", "08662-climate-sensitivity.md"),
            ("earth-climate-environment", "08663-climate-system.md"),
            ("earth-climate-environment", "10827-effects-of-climate-change-on-agriculture.md"),
            ("earth-climate-environment", "25478-water-pollution.md"),
        ],
    },
    {
        "id": "inventors-and-everyday-engineering",
        "name": "Inventors & Everyday Engineering",
        "description": "The machines, materials, structures, circuits, and design ideas behind the built world around us.",
        "articles": [
            ("engineering-everyday-systems", "00041-engineering.md"),
            ("engineering-everyday-systems", "00069-manufacturing.md"),
            ("engineering-everyday-systems", "00096-transport.md"),
            ("engineering-everyday-systems", "00867-aircraft.md"),
            ("engineering-everyday-systems", "00874-bicycle.md"),
            ("engineering-everyday-systems", "00877-bridge.md"),
            ("engineering-everyday-systems", "00891-concrete.md"),
            ("engineering-everyday-systems", "00901-electronics.md"),
            ("engineering-everyday-systems", "00902-engine.md"),
            ("engineering-everyday-systems", "00918-infrastructure.md"),
            ("engineering-everyday-systems", "00920-integrated-circuit.md"),
            ("engineering-everyday-systems", "00921-internal-combustion-engine.md"),
            ("engineering-everyday-systems", "00939-mechanical-engineering.md"),
            ("engineering-everyday-systems", "00961-radio.md"),
            ("engineering-everyday-systems", "00970-semiconductor.md"),
            ("engineering-everyday-systems", "00974-simple-machine.md"),
            ("engineering-everyday-systems", "00979-steam-engine.md"),
            ("engineering-everyday-systems", "00981-telephone.md"),
            ("engineering-everyday-systems", "03116-history-of-transport.md"),
            ("engineering-everyday-systems", "05234-electric-circuit.md"),
            ("engineering-everyday-systems", "05237-transistor.md"),
            ("engineering-everyday-systems", "06382-analytical-engine.md"),
            ("engineering-everyday-systems", "11184-engineering-design-process.md"),
            ("engineering-everyday-systems", "11185-engineering-disasters.md"),
        ],
    },
    {
        "id": "mind-ethics-and-meaning",
        "name": "Mind, Ethics & Meaning",
        "description": "Big questions about consciousness, knowledge, freedom, justice, religion, technology, and how people should live.",
        "articles": [
            ("philosophy-religion-ethics", "00006-philosophy.md"),
            ("philosophy-religion-ethics", "00047-ethics.md"),
            ("philosophy-religion-ethics", "00063-knowledge.md"),
            ("philosophy-religion-ethics", "00067-mind.md"),
            ("philosophy-religion-ethics", "00088-religion.md"),
            ("philosophy-religion-ethics", "00268-consciousness.md"),
            ("philosophy-religion-ethics", "00482-justice.md"),
            ("philosophy-religion-ethics", "00408-history-of-philosophy.md"),
            ("philosophy-religion-ethics", "00850-western-philosophy.md"),
            ("philosophy-religion-ethics", "03060-history-of-islam.md"),
            ("philosophy-religion-ethics", "03042-history-of-buddhism.md"),
            ("philosophy-religion-ethics", "03054-history-of-hinduism.md"),
            ("philosophy-religion-ethics", "04937-nicomachean-ethics.md"),
            ("philosophy-religion-ethics", "05553-ancient-philosophy.md"),
            ("philosophy-religion-ethics", "11188-engineering-ethics.md"),
            ("philosophy-religion-ethics", "11448-ethics-of-artificial-intelligence.md"),
            ("philosophy-religion-ethics", "16212-machine-ethics.md"),
            ("philosophy-religion-ethics", "19088-philosophy-of-mind.md"),
            ("philosophy-religion-ethics", "22535-stanford-encyclopedia-of-philosophy.md"),
            ("philosophy-religion-ethics", "24699-analytic-philosophy.md"),
            ("philosophy-religion-ethics", "24971-free-will.md"),
            ("philosophy-religion-ethics", "24973-freedom-of-religion.md"),
            ("philosophy-religion-ethics", "25231-political-philosophy.md"),
            ("philosophy-religion-ethics", "25409-ethics-of-technology.md"),
        ],
    },
    {
        "id": "world-history-turning-points",
        "name": "World History: Turning Points",
        "description": "A compact timeline of civilizations, ideas, empires, revolutions, and conflicts that reshaped the modern world.",
        "articles": [
            ("history-civilizations", "00001-human-history.md"),
            ("history-civilizations", "00016-ancient-history.md"),
            ("history-civilizations", "00026-civilization.md"),
            ("history-civilizations", "00087-prehistory.md"),
            ("history-civilizations", "00141-ancient-greece.md"),
            ("history-civilizations", "00144-ancient-egypt.md"),
            ("history-civilizations", "00138-ancient-rome.md"),
            ("history-civilizations", "00209-british-empire.md"),
            ("history-civilizations", "00211-byzantine-empire.md"),
            ("history-civilizations", "00250-cold-war.md"),
            ("history-civilizations", "00351-french-revolution.md"),
            ("history-civilizations", "00381-gupta-empire.md"),
            ("history-civilizations", "00441-industrial-revolution.md"),
            ("history-civilizations", "00559-military-history.md"),
            ("history-civilizations", "00595-neolithic-revolution.md"),
            ("history-civilizations", "00687-renaissance.md"),
            ("history-civilizations", "00711-scientific-revolution.md"),
            ("history-civilizations", "00735-silk-road.md"),
            ("history-civilizations", "00858-world-war-i.md"),
            ("history-civilizations", "00859-world-war-ii.md"),
            ("history-civilizations", "03113-history-of-the-united-kingdom.md"),
            ("history-civilizations", "03315-roman-empire.md"),
            ("history-civilizations", "00413-holy-roman-empire.md"),
            ("history-civilizations", "00570-mongol-empire.md"),
        ],
    },
    {
        "id": "creative-arts-and-world-languages",
        "name": "Creative Arts & World Languages",
        "description": "A broad creative shelf covering visual art, music, performance, literature, writing, design, photography, and language.",
        "articles": [
            ("arts-language-culture", "00009-the-arts.md"),
            ("arts-language-culture", "00015-architecture.md"),
            ("arts-language-culture", "00029-culture.md"),
            ("arts-language-culture", "00061-language.md"),
            ("arts-language-culture", "00062-literature.md"),
            ("arts-language-culture", "00074-music.md"),
            ("arts-language-culture", "00080-performing-arts.md"),
            ("arts-language-culture", "00097-writing.md"),
            ("arts-language-culture", "00100-visual-arts.md"),
            ("arts-language-culture", "00159-art.md"),
            ("arts-language-culture", "00280-design.md"),
            ("arts-language-culture", "00315-dance.md"),
            ("arts-language-culture", "00319-english-language.md"),
            ("arts-language-culture", "00339-film.md"),
            ("arts-language-culture", "00352-french-language.md"),
            ("arts-language-culture", "00379-greek-language.md"),
            ("arts-language-culture", "00402-history-of-art.md"),
            ("arts-language-culture", "00404-history-of-literature.md"),
            ("arts-language-culture", "00625-painting.md"),
            ("arts-language-culture", "00637-photography.md"),
            ("arts-language-culture", "00650-poetry.md"),
            ("arts-language-culture", "00803-theatre.md"),
            ("arts-language-culture", "04655-children-s-literature.md"),
            ("arts-language-culture", "04743-epic-poetry.md"),
        ],
    },
]


def create_packs(catalog_version: int) -> None:
    source = json.loads(CATALOG.read_text(encoding="utf-8"))
    existing = {pack["id"] for pack in source["packs"]}
    for definition in PACKS:
        if definition["id"] in existing:
            raise ValueError(f"pack already exists: {definition['id']}")
        target = DB_PACKS / definition["id"]
        target.mkdir(parents=True, exist_ok=True)
        expected = []
        for source_dir, filename in definition["articles"]:
            origin = DB_PACKS / source_dir / filename
            if not origin.is_file():
                raise FileNotFoundError(origin)
            shutil.copy2(origin, target / filename)
            expected.append(filename)
        for stale in target.glob("*.md"):
            if stale.name not in expected:
                stale.unlink()
        source["packs"].append(
            {
                "id": definition["id"],
                "name": definition["name"],
                "version": 1,
                "description": definition["description"],
                "dir": f"db-packs/{definition['id']}",
                "articles": expected,
            }
        )
    source["catalog_version"] = max(int(source["catalog_version"]), catalog_version)
    CATALOG.write_text(json.dumps(source, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"catalog_version": source["catalog_version"], "packs": [p["id"] for p in PACKS]}, indent=2))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog-version", type=int, default=5)
    args = parser.parse_args()
    create_packs(args.catalog_version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
