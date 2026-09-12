#!/usr/bin/env python3
"""Build PocketWiki topical packs from the wiki-distill source database.

The source database is opened read-only.  Each pack is a curated, deterministic
title selection from quality-gated completed distillations, written as Markdown
for the normal PocketWiki pack builder.  The twenty database-backed shelves are
replaced in the catalogue while unrelated hand-curated shelves are preserved.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sqlite3
import unicodedata
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
from article_text import finalize_article
from content_paths import ARTICLES, CATALOG
DEFAULT_DB = Path.home() / "wiki-distill" / "educational-source.db"
DEFAULT_ARTICLES = ARTICLES / "db-packs"
DEFAULT_CATALOG = CATALOG


PACKS = [
    {
        "id": "essential-computing",
        "name": "Essential Computing & Digital Life",
        "description": "Practical foundations for computers, programming, the internet, data, and digital safety.",
        "terms": {
            "computer": 8, "computing": 8, "programming": 8, "algorithm": 8,
            "software": 7, "internet": 7, "network": 7, "database": 8,
            "data": 5, "web": 5, "cryptograph*": 8, "security": 7,
            "artificial intelligence": 8, "machine learning": 8, "operating system": 8,
            "computer science": 8, "information": 3, "digital": 5, "logic": 4,
            "robot": 4, "semiconductor": 5, "microprocessor": 7, "programming language": 8,
        },
        "exact": [
            "Computer", "Computer science", "Algorithm", "Programming language",
            "Internet", "World Wide Web", "Database", "Computer network",
            "Operating system", "Artificial intelligence", "Machine learning",
            "Cryptography", "Computer security", "Digital literacy", "Information",
        ],
        "exclude": [],
    },
    {
        "id": "mathematical-thinking",
        "name": "Mathematical Thinking",
        "description": "A compact toolkit of arithmetic, algebra, geometry, probability, statistics, and mathematical reasoning.",
        "terms": {
            "mathemat*": 8, "algebra": 8, "arithmetic": 8, "geometry": 8,
            "calculus": 8, "statistics": 8, "probability": 8, "number": 5,
            "equation": 7, "theorem": 7, "proof": 7, "set": 5, "function": 6,
            "vector": 6, "matrix": 6, "linear algebra": 8, "discrete": 7,
            "logic": 6, "measurement": 5, "quantity": 4, "topology": 6,
            "trigonometry": 8, "mathematical analysis": 8, "mathematical constant": 7,
        },
        "exact": [
            "Mathematics", "Arithmetic", "Algebra", "Geometry", "Calculus",
            "Statistics", "Probability", "Mathematical proof", "Discrete mathematics",
            "Linear algebra", "Mathematical analysis", "Logic", "Set", "Function (mathematics)",
        ],
        "exclude": ["philosophiæ", "logical calculus"],
    },
    {
        "id": "natural-sciences",
        "name": "Natural Sciences",
        "description": "Core ideas in physics, chemistry, astronomy, and the scientific method for understanding the physical world.",
        "terms": {
            "science": 7, "physics": 8, "chemistry": 8, "chemical": 6, "atom": 8,
            "molecule": 8, "element": 7, "reaction": 6, "energy": 7, "force": 7,
            "motion": 7, "gravity": 8, "light": 6, "wave": 6, "electric": 7,
            "magnet*": 7, "quantum": 8, "relativity": 8, "particle": 6,
            "astronom*": 8, "planet": 7, "universe": 7, "star": 6, "galaxy": 7,
            "scientific method": 9, "experiment": 5, "thermodynamic*": 8,
        },
        "exact": [
            "Science", "Scientific method", "Physics", "Chemistry", "Atom",
            "Chemical element", "Molecule", "Energy", "Force", "Motion",
            "Electricity", "Magnetism", "Quantum mechanics", "Theory of relativity",
            "Astronomy", "Solar System", "Universe", "Thermodynamics",
        ],
        "exclude": ["political science", "social science", "computer science", "abstraction (computer science)"],
    },
    {
        "id": "biology-health",
        "name": "Biology, Health & Medicine",
        "description": "Useful foundations for living systems, the human body, disease, prevention, nutrition, and first aid.",
        "terms": {
            "biolog*": 8, "cell": 7, "gene": 8, "genome": 8, "dna": 8, "rna": 8,
            "organism": 7, "species": 6, "evolution": 8, "animal": 5, "plant": 5,
            "ecology": 6, "anatom*": 8, "physiolog*": 8, "human body": 9, "brain": 6,
            "medic*": 8, "disease": 7, "health": 7, "public health": 9,
            "nutrition": 8, "vaccine*": 8, "virus": 8, "bacteria": 8, "antibiotic": 8,
            "first aid": 10, "mental health": 9, "hygiene": 8, "immune*": 7,
        },
        "exact": [
            "Biology", "Cell (biology)", "DNA", "RNA", "Evolution", "Animal",
            "Plant", "Human body", "Anatomy", "Medicine", "Disease", "Health",
            "Nutrition", "Public health", "Vaccination", "Virus", "Bacteria", "First aid",
        ],
        "exclude": ["medici", "catherine de", "cosimo de", "lorenzo de"],
    },
    {
        "id": "earth-climate-environment",
        "name": "Earth, Climate & Environment",
        "description": "A field guide to Earth systems, weather, oceans, climate change, ecosystems, resources, and sustainability.",
        "terms": {
            "earth": 8, "geolog*": 8, "atmosphere": 8, "climate": 9, "weather": 8,
            "ocean": 7, "water": 6, "river": 5, "glacier": 7, "volcan*": 8,
            "earthquake": 8, "tsunami": 8, "ecosystem": 9, "ecology": 8,
            "biodiversity": 9, "environment": 8, "pollution": 8, "conservation": 7,
            "sustainab*": 8, "renewable energy": 8, "solar energy": 7, "agriculture": 6,
            "soil": 6, "forest": 6, "carbon": 6, "greenhouse": 8, "natural resource": 7,
        },
        "exact": [
            "Earth", "Earth science", "Geology", "Atmosphere of Earth", "Climate",
            "Climate change", "Weather", "Ocean", "Water", "Earthquake", "Volcano",
            "Ecology", "Ecosystem", "Biodiversity", "Environmental issues", "Renewable energy",
        ],
        "exclude": ["lee de forest", "conservation of mass", "conservation of energy"],
    },
    {
        "id": "history-civilizations",
        "name": "History & Civilizations",
        "description": "Big-picture history from prehistory and ancient civilizations through empires, revolutions, and the modern world.",
        "terms": {
            "history": 8, "prehistor*": 8, "ancient": 7, "civilization": 8, "empire": 8,
            "dynasty": 7, "kingdom": 6, "revolution": 8, "war": 6, "battle": 6,
            "roman": 7, "rome": 7, "greek": 7, "egypt": 7, "mesopotamia": 8,
            "medieval": 8, "middle ages": 8, "renaissance": 8, "colonial": 7,
            "cold war": 9, "world war": 9, "holocaust": 8, "archaeolog": 7,
            "pharaoh": 7, "mughal": 8, "ottoman": 8, "mongol": 8, "silk road": 8,
        },
        "exact": [
            "Human history", "Prehistory", "Ancient history", "History", "Civilization",
            "Ancient Rome", "Ancient Greece", "Roman Empire", "Mughal Empire", "Ottoman Empire",
            "Renaissance", "Industrial Revolution", "French Revolution", "Cold War",
            "World War I", "World War II", "Archaeology", "Silk Road",
        ],
        "exclude": [],
    },
    {
        "id": "society-government-economy",
        "name": "Society, Government & Economy",
        "description": "How societies organize power, law, markets, money, institutions, work, education, and public life.",
        "terms": {
            "society": 8, "sociolog*": 8, "government": 9, "democra*": 9, "politic*": 8,
            "constitution": 8, "law": 5, "human rights": 9, "justice": 7, "citizen": 6,
            "econom*": 9, "market": 7, "money": 8, "finance": 8, "bank": 6,
            "trade": 6, "tax": 8, "inflation": 8, "poverty": 8, "inequality": 8,
            "business": 7, "corporat*": 7, "management": 6, "education": 7,
            "population": 6, "migration": 7, "employment": 7, "labor": 6, "media": 6,
        },
        "exact": [
            "Society", "Government", "Democracy", "Politics", "Constitution",
            "Human rights", "Justice", "Economics", "Economy", "Money", "Finance",
            "Bank", "Inflation", "Poverty", "Business", "Education", "Sociology",
            "Supply and demand", "Personal finance", "Tax",
        ],
        "exclude": ["inequality", "inequality", "population genetics", "cosmic inflation", "animal migration", "bird migration", "cauchy", "chebyshev", "fano", "bretagnolle", "concentration inequality", "diamagnetic"],
    },
    {
        "id": "philosophy-religion-ethics",
        "name": "Philosophy, Religion & Ethics",
        "description": "A thoughtful introduction to meaning, knowledge, morality, mind, religion, and major philosophical traditions.",
        "terms": {
            "philosoph*": 9, "ethic*": 9, "religion": 9, "theolog*": 8, "belief": 7,
            "deity": 7, "god": 5, "justice": 7, "truth": 7, "reason": 7,
            "knowledge": 7, "mind": 6, "consciousness": 8, "free will": 9,
            "existential*": 8, "metaphysic*": 8, "epistemolog*": 8, "buddh*": 8,
            "christian": 7, "islam": 7, "hindu*": 7, "quran": 7, "bible": 7,
            "meditation": 7, "secular": 7, "myth": 6, "morality": 8,
        },
        "exact": [
            "Philosophy", "Ethics", "Religion", "Theology", "Ancient philosophy",
            "Western philosophy", "Eastern philosophy", "Justice", "Truth", "Reason",
            "Knowledge", "Mind", "Consciousness", "Free will", "Buddhism", "Christianity",
            "Islam", "Hinduism", "Quran", "Bible",
        ],
        "exclude": ["hans christian", "sojourner truth", "hindu kush", "hindustani language"],
    },
    {
        "id": "arts-language-culture",
        "name": "Arts, Language & Culture",
        "description": "Explore how people communicate and create through language, writing, literature, music, visual art, film, and design.",
        "terms": {
            "art": 8, "music": 8, "literature": 8, "language": 8, "writing": 8,
            "poetry": 8, "novel": 7, "theatr*": 8, "film": 8,
            "cinema": 8, "photograph*": 8, "painting": 8, "sculpture": 8,
            "architecture": 8, "dance": 7, "design": 6, "culture": 8,
            "folklore": 8, "myth": 6, "fashion": 7, "publishing": 7, "media": 5,
            "story": 5, "script": 5, "rhetoric": 7, "translation": 7,
        },
        "exact": [
            "The arts", "Art", "Visual arts", "Performing arts", "Music", "Literature",
            "Language", "Writing", "Poetry", "Novel", "Photography", "Painting",
            "Sculpture", "Architecture", "Film", "Culture", "Folklore", "Mythology",
        ],
        "exclude": ["programming", "computer", "data-centric", "data-driven", "language model", "bacterial", "blood culture", "cell culture", "microbiological culture", "fed-batch culture", "cache language", "assembly language", "scripting language", "domain-specific language", "modeling language", "query language", "cognitive architecture", "arm architecture", "genetic architecture", "ibm power architecture", "industry standard architecture", "mips architecture", "mamba", "contrastive language", "artificial intelligence", "film capacitor"],
    },
    {
        "id": "engineering-everyday-systems",
        "name": "Engineering & Everyday Systems",
        "description": "Understand the designed world: materials, machines, buildings, energy, transport, electronics, and infrastructure.",
        "terms": {
            "engineer*": 9, "machine": 7, "engine": 8, "mechanical": 8, "electrical": 8,
            "electronic*": 8, "circuit": 8, "semiconductor": 8, "transistor": 8,
            "battery": 8, "energy": 6, "material": 7, "steel": 6, "concrete": 7,
            "construction": 7, "building": 6, "bridge": 8, "infrastructure": 8,
            "transport*": 8, "automobile": 7, "car": 4, "bicycle": 7, "railway": 7,
            "aircraft": 7, "aerospace": 7, "agriculture": 5, "manufactur*": 8,
            "water supply": 8, "sanitation": 8, "telephone": 7, "radio": 6,
        },
        "exact": [
            "Engineering", "Mechanical engineering", "Electrical engineering", "Electronics",
            "Machine", "Engine", "Electricity", "Circuit", "Semiconductor", "Transistor",
            "Battery", "Architecture", "Bridge", "Infrastructure", "Transportation",
            "Bicycle", "Manufacturing", "Telephone", "Radio", "Sanitation",
        ],
        "exclude": [],
    },
]

# Focused follow-up shelves. These use new stable IDs so a release adds ten
# genuinely new catalogue entries instead of silently changing old pack names.
PACKS += [
    {
        "id": "living-world", "name": "The Living World",
        "description": "A lively field guide to cells, genes, animals, plants, evolution, ecosystems, and the diversity of life.",
        "terms": {"biolog*": 9, "cell": 8, "gene": 8, "genome": 8, "dna": 8, "rna": 8, "organism": 8, "species": 8, "evolution": 9, "animal": 6, "plant": 6, "ecology": 8, "ecosystem": 8, "biodiversity": 9, "botan*": 8, "zoolog*": 8, "microbiolog*": 8, "physiolog*": 7, "anatom*": 6, "virus": 6, "bacteria": 7, "fung*": 7},
        "exact": ["Life", "Biology", "Cell (biology)", "DNA", "Gene", "Genome", "Evolution", "Animal", "Plant", "Species", "Ecology", "Ecosystem", "Biodiversity", "Natural selection", "Microbiology"],
        "exclude": ["social ecology", "human ecology", "ecological economics"],
    },
    {
        "id": "mind-and-behavior", "name": "Mind & Behaviour",
        "description": "Explore perception, memory, learning, emotion, personality, consciousness, development, and the science of behaviour.",
        "terms": {"psycholog*": 9, "cognit*": 9, "brain": 8, "memory": 9, "perception": 8, "emotion": 8, "behavio*": 9, "learning": 8, "consciousness": 8, "personality": 8, "intelligence": 8, "neuro*": 8, "motivation": 7, "developmental": 7, "mental health": 8, "mind": 6, "dream": 7, "attention": 8, "decision": 6},
        "exact": ["Mind", "Brain", "Psychology", "Cognitive science", "Memory", "Learning", "Perception", "Emotion", "Consciousness", "Personality", "Intelligence", "Developmental psychology", "Mental health", "Working memory", "Attention"],
        "exclude": ["machine learning", "neural network", "artificial intelligence", "computer vision", "animal learning"],
    },
    {
        "id": "cosmos-and-spaceflight", "name": "Cosmos & Spaceflight",
        "description": "Travel from the solar system to distant galaxies through stars, rockets, orbits, cosmology, and space exploration.",
        "terms": {"astronom*": 9, "cosmolog*": 9, "space": 7, "planet": 8, "star": 8, "galaxy": 8, "universe": 9, "solar system": 9, "orbit": 8, "rocket": 8, "spaceflight": 10, "spacecraft": 9, "satellite": 7, "telescope": 8, "gravity": 7, "relativity": 7, "black hole": 9, "nebula": 8, "moon": 7, "mars": 7, "astrophysic*": 9, "exoplanet": 9},
        "exact": ["Astronomy", "Solar System", "Universe", "Planet", "Star", "Galaxy", "Gravity", "Spaceflight", "Rocket", "Moon", "Mars", "Black hole", "Astrophysics", "Telescope", "Exoplanet"],
        "exclude": ["space (mathematics)", "three-dimensional space", "space syntax"],
    },
    {
        "id": "oceans-weather-and-earth", "name": "Oceans, Weather & Earth",
        "description": "Understand the restless planet through oceans, rivers, rocks, weather, volcanoes, earthquakes, and Earth-shaping forces.",
        "terms": {"ocean": 9, "sea": 7, "marine": 8, "water": 7, "river": 7, "lake": 7, "weather": 9, "meteorolog*": 9, "climate": 7, "atmosphere": 8, "geolog*": 9, "earth": 7, "volcan*": 9, "earthquake": 9, "tsunami": 9, "glacier": 8, "rock": 6, "mineral": 7, "tectonic": 9, "flood": 7, "coast": 7, "soil": 6, "hydrolog*": 8},
        "exact": ["Earth", "Ocean", "Water", "River", "Atmosphere of Earth", "Weather", "Climate", "Geology", "Volcano", "Earthquake", "Tsunami", "Glacier", "Mineral", "Plate tectonics", "Water cycle"],
        "exclude": ["earth (element)", "earth (classical element)", "water (journal)", "water polo"],
    },
    {
        "id": "ancient-worlds-and-archaeology", "name": "Ancient Worlds & Archaeology",
        "description": "Meet the cities, rulers, religions, technologies, and archaeological clues of the ancient world.",
        "terms": {"ancient": 9, "archaeolog*": 10, "prehistor*": 9, "civilization": 9, "empire": 8, "dynasty": 8, "pharaoh": 9, "mesopotamia": 10, "egypt": 8, "greek": 7, "roman": 7, "rome": 7, "babylon": 9, "persian": 7, "mycenaean": 9, "minoan": 9, "bronze age": 9, "iron age": 8, "neolithic": 9, "sumer": 9, "maya": 7, "inca": 7, "aztec": 7},
        "exact": ["Ancient history", "Prehistory", "Ancient Egypt", "Ancient Greece", "Ancient Rome", "Civilization", "Archaeology", "Mesopotamia", "Roman Empire", "Bronze Age", "Neolithic Revolution", "Maya civilization", "Inca Empire", "Sumer"],
        "exclude": ["ancient philosophy", "ancient Greek astronomy", "ancient Greek architecture", "ancient Greek art", "ancient Greek literature"],
    },
    {
        "id": "ideas-ethics-and-society", "name": "Ideas, Ethics & Society",
        "description": "A conversation across philosophy, religion, justice, rights, reason, politics, and ideas that shape public life.",
        "terms": {"philosoph*": 9, "ethic*": 9, "morality": 9, "religion": 8, "theolog*": 8, "justice": 9, "human rights": 10, "freedom": 7, "truth": 7, "reason": 7, "knowledge": 8, "belief": 7, "consciousness": 7, "free will": 9, "democra*": 7, "political philosophy": 10, "social contract": 9, "secular": 7, "myth": 6, "worldview": 7, "religious": 7},
        "exact": ["Philosophy", "Ethics", "Morality", "Religion", "Justice", "Human rights", "Knowledge", "Truth", "Reason", "Free will", "Political philosophy", "Social contract", "Consciousness", "Worldview", "Secularism"],
        "exclude": ["engineering ethics", "machine ethics", "ethics of artificial intelligence", "ethics of technology", "political science"],
    },
    {
        "id": "story-language-and-media", "name": "Story, Language & Media",
        "description": "See how humans make meaning through language, writing, literature, storytelling, journalism, film, and mass media.",
        "terms": {"language": 9, "linguist*": 9, "writing": 9, "literature": 9, "story": 8, "narrat*": 8, "novel": 8, "poetry": 8, "theatr*": 7, "film": 8, "cinema": 8, "media": 8, "communication": 8, "journalism": 9, "rhetoric": 8, "translation": 8, "publishing": 8, "script": 6, "folklore": 7, "myth": 6, "semiotic*": 8},
        "exact": ["Language", "Writing", "Literature", "Communication", "Storytelling", "Novel", "Poetry", "Film", "Theatre", "Mass media", "Journalism", "Translation", "Publishing", "Rhetoric", "Folklore"],
        "exclude": ["programming language", "computer language", "machine language", "assembly language", "query language", "modeling language", "formal language", "language model", "body language"],
    },
    {
        "id": "music-art-and-design", "name": "Music, Art & Design",
        "description": "A creative atlas of music, painting, sculpture, photography, architecture, dance, visual culture, and design.",
        "terms": {"art": 9, "music": 9, "painting": 9, "sculpture": 9, "photograph*": 9, "architecture": 8, "design": 8, "dance": 8, "theatr*": 7, "film": 7, "cinema": 7, "drawing": 8, "printmaking": 9, "fashion": 7, "composer": 8, "musical": 7, "instrument": 7, "aesthetic": 7, "museum": 7, "portrait": 8, "illustration": 8},
        "exact": ["The arts", "Art", "Visual arts", "Performing arts", "Music", "Painting", "Sculpture", "Photography", "Architecture", "Design", "Dance", "Film", "Drawing", "Fashion", "Museum"],
        "exclude": ["computer graphics", "film capacitor", "genetic architecture", "software architecture", "network architecture"],
    },
    {
        "id": "money-markets-and-work", "name": "Money, Markets & Work",
        "description": "Follow the flow of money through markets, banks, trade, businesses, labour, taxation, and the working world.",
        "terms": {"econom*": 9, "market": 8, "money": 9, "finance": 9, "bank": 8, "business": 8, "trade": 8, "tax": 8, "inflation": 9, "poverty": 8, "inequality": 8, "wealth": 8, "capital": 7, "labor": 8, "labour": 8, "employment": 8, "industry": 7, "commerce": 8, "corporat*": 7, "investment": 8, "accounting": 8, "supply and demand": 10},
        "exact": ["Economics", "Economy", "Money", "Finance", "Bank", "Business", "Markets", "Trade", "Inflation", "Poverty", "Wealth", "Tax", "Employment", "Labour", "Supply and demand"],
        "exclude": ["cosmic inflation", "inflation (physics)", "population genetics", "animal migration", "concentration inequality", "engineering economics"],
    },
    {
        "id": "machines-materials-and-infrastructure", "name": "Machines, Materials & Infrastructure",
        "description": "Decode the built world through machines, engines, electronics, materials, transport, construction, and infrastructure.",
        "terms": {"engineer*": 9, "machine": 8, "engine": 9, "mechanical": 8, "electrical": 8, "electronic*": 8, "circuit": 8, "semiconductor": 9, "transistor": 9, "battery": 8, "material": 8, "steel": 8, "concrete": 8, "construction": 8, "building": 7, "bridge": 8, "infrastructure": 9, "transport*": 8, "manufactur*": 9, "railway": 8, "aircraft": 8, "automobile": 7, "telephone": 7, "radio": 7, "water supply": 8, "sanitation": 8},
        "exact": ["Engineering", "Machine", "Engine", "Mechanical engineering", "Electrical engineering", "Electronics", "Circuit", "Semiconductor", "Transistor", "Battery", "Materials science", "Steel", "Concrete", "Bridge", "Infrastructure", "Transportation", "Manufacturing"],
        "exclude": ["computer engineering", "software engineering", "engineering ethics", "engineering economics", "engineering education", "genetic engineering"],
    },
]


def norm(text: str) -> str:
    text = unicodedata.normalize("NFKC", text).casefold()
    return re.sub(r"\s+", " ", text).strip()


def title_matches(title: str, term: str) -> bool:
    """Match whole title words/phrases; a trailing * explicitly means prefix."""
    if term.endswith("*"):
        return re.search(rf"(?<!\w){re.escape(term[:-1])}\w*", title) is not None
    return re.search(rf"(?<!\w){re.escape(term)}(?!\w)", title) is not None


def slug(text: str) -> str:
    ascii_text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode("ascii")
    value = re.sub(r"[^a-z0-9]+", "-", ascii_text.casefold()).strip("-")
    return value or "article"


def select_articles(rows: list[sqlite3.Row], spec: dict, target: int = 96) -> list[sqlite3.Row]:
    by_title = {norm(row["title"]): row for row in rows}
    selected: dict[int, sqlite3.Row] = {}
    excluded = [norm(term) for term in spec.get("exclude", [])]
    for title in spec["exact"]:
        row = by_title.get(norm(title))
        if row is not None and not any(term in norm(row["title"]) for term in excluded):
            selected[int(row["id"])] = row

    scored: list[tuple[int, int, sqlite3.Row]] = []
    terms = [(norm(term), weight) for term, weight in spec["terms"].items()]
    for row in rows:
        title = norm(row["title"])
        if any(term in title for term in excluded):
            continue
        score = sum(weight for term, weight in terms if title_matches(title, term))
        if score:
            # Lower source ids are generally the project's more foundational articles.
            score += max(0, 4 - int(row["id"]) // 2000)
            scored.append((score, int(row["id"]), row))
    scored.sort(key=lambda item: (-item[0], item[1]))
    for _score, _id, row in scored:
        selected.setdefault(int(row["id"]), row)
        if len(selected) >= target:
            break
    if len(selected) < target:
        raise RuntimeError(f"{spec['id']}: only found {len(selected)} matching articles")
    return sorted(selected.values(), key=lambda row: int(row["id"]))[:target]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--db", type=Path, default=DEFAULT_DB)
    parser.add_argument("--articles-root", type=Path, default=DEFAULT_ARTICLES)
    parser.add_argument("--catalog", type=Path, default=DEFAULT_CATALOG)
    parser.add_argument(
        "--catalog-version", type=int, default=None,
        help="new public catalogue pointer version (otherwise keep the source version)",
    )
    parser.add_argument(
        "--articles-per-pack", type=int, default=100,
        help="number of curated articles in each database-backed pack",
    )
    args = parser.parse_args()

    if not args.db.is_file():
        raise FileNotFoundError(args.db)
    args.articles_root.mkdir(parents=True, exist_ok=True)

    uri = f"file:{args.db.resolve()}?mode=ro"
    with sqlite3.connect(uri, uri=True) as conn:
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA query_only=ON")
        rows = conn.execute(
            """
            SELECT a.id, a.title, m.draft, m.words
            FROM articles a
            JOIN minimax_distillations m ON m.article_id = a.id
            WHERE a.status='complete'
              AND m.status='complete'
              AND m.quality_status='complete'
              AND length(trim(COALESCE(m.draft, ''))) > 100
            ORDER BY a.id
            """
        ).fetchall()

    source = json.loads(args.catalog.read_text(encoding="utf-8"))
    if source.get("schema") != 1 or not isinstance(source.get("packs"), list):
        raise ValueError("catalog source must use schema 1 and contain packs")
    managed_ids = {spec["id"] for spec in PACKS}
    existing_by_id = {pack["id"]: pack for pack in source["packs"]}
    # Keep other shelves (for example UK curriculum and special-interest packs)
    # in their existing order. The ten database-backed shelves are reinserted
    # at their original leading position below.
    catalog_packs = [
        pack for pack in source["packs"] if pack["id"] not in managed_ids
    ]
    rebuilt_managed = []
    selection_report = {"db": str(args.db), "packs": []}
    for spec in PACKS:
        pack_dir = args.articles_root / spec["id"]
        if pack_dir.exists():
            shutil.rmtree(pack_dir)
        pack_dir.mkdir(parents=True, exist_ok=True)
        selected = select_articles(rows, spec, args.articles_per_pack)
        filenames = []
        for row in selected:
            filename = f"{int(row['id']):05d}-{slug(row['title'])}.md"
            text = (row["draft"] or "").strip()
            # Normalize the leading title so the pack index always reflects DB metadata.
            text = re.sub(r"^#\s+[^\n]*(?:\n+|$)", "", text, count=1)
            body, _ = finalize_article(f"# {row['title']}\n\n{text.strip()}\n")
            (pack_dir / filename).write_text(body, encoding="utf-8")
            filenames.append(filename)
        report = {
            "id": spec["id"],
            "name": spec["name"],
            "articles": len(selected),
            "article_ids": [int(row["id"]) for row in selected],
            "titles": [row["title"] for row in selected],
            "words": sum(int(row["words"] or 0) for row in selected),
        }
        (pack_dir / "selection.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        selection_report["packs"].append(report)
        old = existing_by_id.get(spec["id"])
        version = int(old["version"]) if old is not None else 1
        if old is not None and old.get("articles") != filenames:
            version += 1
        rebuilt_managed.append({
            "id": spec["id"],
            "name": spec["name"],
            "version": version,
            "description": spec["description"],
            "dir": f"db-packs/{spec['id']}",
            "articles": filenames,
        })

    # Keep the established first-ten ordering while preserving unrelated
    # entries that were already in the source catalogue.
    catalog_packs = rebuilt_managed + catalog_packs
    requested_version = (
        int(args.catalog_version)
        if args.catalog_version is not None
        else int(source.get("catalog_version", 1))
    )
    args.catalog.write_text(
        json.dumps({
            "schema": 1,
            "catalog_version": max(int(source.get("catalog_version", 1)), requested_version),
            "base_url": "https://packs.educated.space",
            "packs": catalog_packs,
        }, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    report_path = args.articles_root / "selection-report.json"
    report_path.write_text(json.dumps(selection_report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"db_articles": len(rows), "packs": [{"id": p["id"], "articles": p["articles"], "words": p["words"]} for p in selection_report["packs"]]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
