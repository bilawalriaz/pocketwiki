#!/usr/bin/env python3
"""Build the offline catalogue browser, `packs.html` in the content checkout.

The page is one self-contained file. It embeds the catalogue, the clustered
shelf metadata, and each pack's article titles, so it opens from `file://` with
no network and no server.

Install sizes are exact: every pack is built here with the same builder the
publisher uses (`tools/build_pack_catalog.py`), which also rejects a pack that
would exceed the device's upload limit.

    python3 tools/build_pack_browser.py
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from build_pack_catalog import build_catalog  # noqa: E402
import content_paths  # noqa: E402

TITLE_RE = re.compile(r"^\s*#\s+(.+?)\s*$", re.MULTILINE)

DEFAULT_CATALOG = content_paths.CATALOG
DEFAULT_MANIFEST = content_paths.SHELF_MANIFEST
# The browser is a reader-facing view of the catalogue, so it is written next to
# the content it describes.
DEFAULT_OUTPUT = content_paths.CATALOGUE_BROWSER

# Pack-store capacity on each supported board, from firmware/partitions*.csv.
STORE_BYTES = {"c3": 0x1F0000, "s3": 0xDE0000}

CLUSTERED_DIR = "cluster-packs"


def curated_titles(articles_dir: str) -> list[str]:
    """Article titles for a hand-curated pack, read from its Markdown H1s."""
    directory = content_paths.article_dir(articles_dir)
    titles = []
    for path in sorted(directory.glob("*.md")):
        match = TITLE_RE.search(path.read_text(encoding="utf-8"))
        titles.append(match.group(1).strip() if match else path.stem)
    return titles


def collect(catalog_source: Path, manifest_path: Path) -> dict:
    """Build every pack and merge it with the clustered-shelf metadata."""
    manifest = {}
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    shelves = {pack["id"]: pack for pack in manifest.get("packs", [])}

    source = json.loads(catalog_source.read_text(encoding="utf-8"))
    directories = {pack["id"]: pack.get("dir", "starter") for pack in source["packs"]}

    with tempfile.TemporaryDirectory(prefix="pocketwiki-browser-") as temp:
        built = build_catalog(catalog_source, Path(temp))

    collections: dict[str, dict] = {}
    packs = []
    for entry in built["packs"]:
        shelf = shelves.get(entry["id"])
        collection_id = shelf["collection"] if shelf else "curated"
        kind = "clustered" if directories[entry["id"]].startswith(f"{CLUSTERED_DIR}/") else "curated"
        titles = shelf["titles"] if shelf else curated_titles(directories[entry["id"]])
        packs.append({
            "id": entry["id"],
            "name": entry["name"],
            "description": entry["description"],
            "kind": kind,
            "collection": collection_id,
            "version": entry["version"],
            "articles": entry["articles"],
            "bytes": entry["bytes"],
            "url": entry["url"],
            "titles": titles,
            "words": shelf["words"] if shelf else 0,
        })
        bucket = collections.setdefault(collection_id, {
            "id": collection_id,
            "name": "Curated shelves" if collection_id == "curated" else collection_id,
            "description": "Hand-picked shelves built from title terms rather than embeddings."
                           if collection_id == "curated" else "",
            "packs": 0,
            "articles": 0,
            "bytes": 0,
        })
        bucket["packs"] += 1
        bucket["articles"] += entry["articles"]
        bucket["bytes"] += entry["bytes"]

    for collection in manifest.get("collections", []):
        bucket = collections.get(collection["id"])
        if bucket is None:
            continue
        bucket["name"] = collection["name"]
        bucket["description"] = collection["description"]
        bucket["articles"] = collection["articles"]

    order = {collection["id"]: index for index, collection in enumerate(manifest.get("collections", []))}
    ordered = sorted(
        collections.values(),
        key=lambda item: (order.get(item["id"], len(order)), -item["articles"]),
    )
    return {
        "schema": 1,
        "catalog_version": built["catalog_version"],
        "base_url": json.loads(catalog_source.read_text(encoding="utf-8"))["base_url"],
        "store_bytes": STORE_BYTES,
        "collections": ordered,
        "packs": packs,
    }


def render(data: dict) -> str:
    payload = json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")
    total_bytes = sum(pack["bytes"] for pack in data["packs"])
    total_articles = sum(pack["articles"] for pack in data["packs"])
    return PAGE.replace("__PACK_DATA__", payload) \
               .replace("__CATALOG_VERSION__", str(data["catalog_version"])) \
               .replace("__PACK_COUNT__", str(len(data["packs"]))) \
               .replace("__ARTICLE_COUNT__", str(total_articles)) \
               .replace("__TOTAL_BYTES__", str(total_bytes))


PAGE = r"""<!DOCTYPE html>
<html lang="en" data-theme="dark">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>PocketWiki catalogue browser</title>
<meta name="description" content="Browse and sort every PocketWiki pack: clustered shelves, curated shelves, article counts, and exact install sizes.">
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'%3E%3Crect width='64' height='64' rx='14' fill='%23000000'/%3E%3Ctext x='32' y='45' font-family='system-ui,sans-serif' font-size='40' font-weight='700' fill='%232db89a' text-anchor='middle'%3EP%3C/text%3E%3C/svg%3E">
<script>
try {
  var stored = localStorage.getItem('pocketwiki-theme');
  if (stored) document.documentElement.dataset.theme = stored;
} catch (e) { /* storage unavailable */ }
</script>
<style>
:root {
  --font-sans: ui-sans-serif, system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif;
  --font-mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  --r-md: 6px; --r-lg: 8px; --r-xl: 12px; --r-full: 999px;
  --dur-quick: 150ms; --ease-out: cubic-bezier(0.32, 0.72, 0, 1);
}
[data-theme="dark"] {
  --bg: #000000; --surface: #181818; --card: #1F1F1F; --hover: #272727;
  --border: #313131; --text: #f5f5f7; --text-2: #a1a1a6; --text-3: #8e8e93;
  --accent: #2db89a; --input: #313131; --code-surface: #141414;
  --shadow: 0 8px 32px rgba(0, 0, 0, 0.28);
}
[data-theme="light"] {
  --bg: #f5f5f7; --surface: #ffffff; --card: #ffffff; --hover: #f5f5f7;
  --border: #e3e3e6; --text: #1d1d1f; --text-2: #58585d; --text-3: #6e6e73;
  --accent: #12705c; --input: #e3e3e6; --code-surface: #f2f2f4;
  --shadow: 0 8px 32px rgba(0, 0, 0, 0.08);
}
*, *::before, *::after { box-sizing: border-box; }
[hidden] { display: none !important; }
html { -webkit-text-size-adjust: 100%; }
body {
  margin: 0; background: var(--bg); color: var(--text);
  font: 15px/1.6 var(--font-sans);
}
a { color: var(--accent); }
code, .mono { font-family: var(--font-mono); font-size: 12px; }
mark { background: var(--accent); color: #04120e; border-radius: 2px; padding: 0 1px; }

header {
  position: sticky; top: 0; z-index: 5;
  background: var(--surface); border-bottom: 1px solid var(--border);
  padding: 18px 24px 14px;
}
.head-row { display: flex; align-items: baseline; gap: 12px; flex-wrap: wrap; }
h1 { font-size: 19px; margin: 0; letter-spacing: -0.01em; }
.sub { color: var(--text-2); font-size: 13px; }
.sub b { color: var(--text); font-weight: 600; }
.theme-btn {
  margin-left: auto; background: transparent; color: var(--text-2);
  border: 1px solid var(--border); border-radius: var(--r-full);
  padding: 5px 12px; font: inherit; font-size: 13px; cursor: pointer;
}
.theme-btn:hover { background: var(--hover); color: var(--text); }
.toolbar {
  display: flex; flex-wrap: wrap; gap: 8px; align-items: center; margin-top: 14px;
}
input[type="search"], select {
  background: var(--input); color: var(--text); border: 1px solid var(--border);
  border-radius: var(--r-md); padding: 8px 10px; font: inherit; font-size: 14px;
}
input[type="search"] { min-width: 260px; flex: 1 1 260px; }
input[type="search"]:focus, select:focus { outline: 2px solid var(--accent); outline-offset: 1px; }
label.check { display: inline-flex; align-items: center; gap: 6px; color: var(--text-2); font-size: 13px; }
.count { color: var(--text-2); font-size: 13px; margin-top: 10px; }
.count b { color: var(--text); }

main { padding: 18px 24px 64px; max-width: 1180px; margin: 0 auto; }
.collection { margin: 0 0 26px; }
.collection-head { border-bottom: 1px solid var(--border); padding-bottom: 8px; margin-bottom: 12px; }
.collection-head h2 { font-size: 15px; margin: 0 0 2px; }
.collection-head p { margin: 0; color: var(--text-2); font-size: 13px; }
.collection-head .meta { color: var(--text-3); font-size: 12px; margin-top: 4px; }
.packs { display: grid; gap: 10px; }

.pack {
  background: var(--card); border: 1px solid var(--border); border-radius: var(--r-xl);
  padding: 14px 16px;
}
.pack.curated { border-left: 3px solid var(--accent); }
.pack-head { display: flex; align-items: baseline; gap: 10px; flex-wrap: wrap; }
.pack-head h3 { font-size: 15px; margin: 0; font-weight: 650; }
.chip {
  font-size: 11px; text-transform: uppercase; letter-spacing: 0.06em;
  color: var(--text-2); border: 1px solid var(--border); border-radius: var(--r-full);
  padding: 1px 8px;
}
.chip.curated { color: var(--accent); border-color: var(--accent); }
.pack-desc { color: var(--text-2); font-size: 13.5px; margin: 6px 0 8px; }
.pack-meta { display: flex; gap: 14px; flex-wrap: wrap; color: var(--text-3); font-size: 12px; }
.pack-meta span b { color: var(--text-2); font-weight: 600; }
details { margin-top: 10px; }
summary { cursor: pointer; color: var(--accent); font-size: 13px; }
summary::marker { color: var(--text-3); }
.titles { margin: 10px 0 0; padding: 0; list-style: none; columns: 2; column-gap: 26px; }
@media (max-width: 720px) { .titles { columns: 1; } main, header { padding-left: 14px; padding-right: 14px; } }
.titles li { break-inside: avoid; font-size: 13px; color: var(--text-2); padding: 2px 0; }
.titles li span.i { color: var(--text-3); font-family: var(--font-mono); font-size: 11px; margin-right: 6px; }
.empty { color: var(--text-2); padding: 40px 0; text-align: center; }
footer { color: var(--text-3); font-size: 12px; padding: 0 24px 40px; max-width: 1180px; margin: 0 auto; }
</style>
</head>
<body>
<header>
  <div class="head-row">
    <h1>PocketWiki catalogue</h1>
    <span class="sub">catalogue v__CATALOG_VERSION__ &middot; <b>__PACK_COUNT__</b> packs &middot;
      <b>__ARTICLE_COUNT__</b> articles &middot; <b id="total-bytes"></b> total</span>
    <button class="theme-btn" id="theme" type="button">Light mode</button>
  </div>
  <div class="toolbar">
    <input type="search" id="q" placeholder="Search packs, descriptions, and article titles&hellip;" autocomplete="off" spellcheck="false">
    <select id="collection"><option value="">All collections</option></select>
    <select id="kind">
      <option value="">All packs</option>
      <option value="clustered">Clustered shelves</option>
      <option value="curated">Curated shelves</option>
    </select>
    <select id="sort">
      <option value="catalogue">Catalogue order</option>
      <option value="name">Name A &rarr; Z</option>
      <option value="name-desc">Name Z &rarr; A</option>
      <option value="articles">Most articles</option>
      <option value="articles-asc">Fewest articles</option>
      <option value="bytes">Largest install</option>
      <option value="bytes-asc">Smallest install</option>
    </select>
    <label class="check"><input type="checkbox" id="group" checked> Group by collection</label>
  </div>
  <div class="count" id="count"></div>
</header>
<main id="results"></main>
<footer>
  Sizes are the exact <code>.pwp</code> bytes the publisher uploads. Store capacity is 1.9&nbsp;MB on
  the 4&nbsp;MB ESP32-C3 and 13.9&nbsp;MB on the 16&nbsp;MB ESP32-S3. Generated by
  <code>tools/build_pack_browser.py</code>.
</footer>

<script type="application/json" id="pack-data">__PACK_DATA__</script>
<script>
(function () {
  'use strict';
  var data = JSON.parse(document.getElementById('pack-data').textContent);
  var packs = data.packs;
  var collections = {};
  data.collections.forEach(function (collection) { collections[collection.id] = collection; });

  var order = {};
  packs.forEach(function (pack, index) { order[pack.id] = index; });
  packs.forEach(function (pack) {
    pack.haystack = (pack.name + '\n' + pack.description + '\n' + pack.id + '\n' +
      (collections[pack.collection] || {}).name + '\n' + pack.titles.join('\n')).toLowerCase();
    pack.titlesLower = pack.titles.map(function (title) { return title.toLowerCase(); });
  });

  var els = {
    q: document.getElementById('q'),
    collection: document.getElementById('collection'),
    kind: document.getElementById('kind'),
    sort: document.getElementById('sort'),
    group: document.getElementById('group'),
    count: document.getElementById('count'),
    results: document.getElementById('results'),
    theme: document.getElementById('theme'),
    total: document.getElementById('total-bytes')
  };

  function bytes(value) {
    if (value >= 1048576) return (value / 1048576).toFixed(2) + ' MB';
    return (value / 1024).toFixed(1) + ' KB';
  }
  function share(value) { return (value / data.store_bytes.c3 * 100).toFixed(1) + '%'; }
  function escapeHtml(text) {
    return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }
  function highlight(text, query) {
    if (!query) return escapeHtml(text);
    var lower = text.toLowerCase(), at = lower.indexOf(query), out = '', from = 0;
    while (at !== -1) {
      out += escapeHtml(text.slice(from, at)) + '<mark>' +
             escapeHtml(text.slice(at, at + query.length)) + '</mark>';
      from = at + query.length;
      at = lower.indexOf(query, from);
    }
    return out + escapeHtml(text.slice(from));
  }

  data.collections.forEach(function (collection) {
    var option = document.createElement('option');
    option.value = collection.id;
    option.textContent = collection.name + ' (' + collection.packs + ')';
    els.collection.appendChild(option);
  });

  var sorters = {
    'catalogue': function (a, b) { return order[a.id] - order[b.id]; },
    'name': function (a, b) { return a.name.localeCompare(b.name); },
    'name-desc': function (a, b) { return b.name.localeCompare(a.name); },
    'articles': function (a, b) { return b.articles - a.articles || a.name.localeCompare(b.name); },
    'articles-asc': function (a, b) { return a.articles - b.articles || a.name.localeCompare(b.name); },
    'bytes': function (a, b) { return b.bytes - a.bytes || a.name.localeCompare(b.name); },
    'bytes-asc': function (a, b) { return a.bytes - b.bytes || a.name.localeCompare(b.name); }
  };

  function selected() {
    var query = els.q.value.trim().toLowerCase();
    var collection = els.collection.value;
    var kind = els.kind.value;
    var found = packs.filter(function (pack) {
      if (collection && pack.collection !== collection) return false;
      if (kind && pack.kind !== kind) return false;
      return !query || pack.haystack.indexOf(query) !== -1;
    });
    found.sort(sorters[els.sort.value] || sorters.catalogue);
    return { query: query, packs: found };
  }

  function packCard(pack, query) {
    var card = document.createElement('article');
    card.className = 'pack' + (pack.kind === 'curated' ? ' curated' : '');
    var matching = query ? pack.titlesLower.filter(function (title) {
      return title.indexOf(query) !== -1;
    }).length : 0;
    card.innerHTML =
      '<div class="pack-head">' +
        '<h3>' + highlight(pack.name, query) + '</h3>' +
        '<span class="chip' + (pack.kind === 'curated' ? ' curated' : '') + '">' +
          (pack.kind === 'curated' ? 'curated' : 'clustered') + '</span>' +
        '<span class="chip">' + escapeHtml((collections[pack.collection] || {}).name || '') + '</span>' +
      '</div>' +
      '<p class="pack-desc">' + highlight(pack.description, query) + '</p>' +
      '<div class="pack-meta">' +
        '<span><b>' + pack.articles + '</b> articles</span>' +
        '<span><b>' + bytes(pack.bytes) + '</b> install</span>' +
        '<span><b>' + share(pack.bytes) + '</b> of a C3 store</span>' +
        (pack.words ? '<span><b>' + pack.words.toLocaleString() + '</b> words</span>' : '') +
        '<span>v' + pack.version + '</span>' +
        '<span class="mono">' + escapeHtml(pack.id) + '</span>' +
      '</div>';

    var details = document.createElement('details');
    var summary = document.createElement('summary');
    var label = pack.titles.length + ' article titles';
    if (query && matching !== pack.titles.length) label = matching + ' of ' + pack.titles.length + ' titles match';
    summary.textContent = label;
    details.appendChild(summary);
    details.addEventListener('toggle', function () {
      if (!details.open || details.dataset.filled) return;
      details.dataset.filled = '1';
      var list = document.createElement('ul');
      list.className = 'titles';
      pack.titles.forEach(function (title, index) {
        if (query && pack.titlesLower[index].indexOf(query) === -1) return;
        var item = document.createElement('li');
        item.innerHTML = '<span class="i">' + String(index + 1).padStart(2, '0') + '</span>' +
                         highlight(title, query);
        list.appendChild(item);
      });
      details.appendChild(list);
    });
    if (pack.titles.length) card.appendChild(details);
    return card;
  }

  function render() {
    var state = selected();
    var query = state.query;
    var shown = state.packs;
    var articles = shown.reduce(function (sum, pack) { return sum + pack.articles; }, 0);
    var size = shown.reduce(function (sum, pack) { return sum + pack.bytes; }, 0);
    els.count.innerHTML = '<b>' + shown.length + '</b> of ' + packs.length + ' packs &middot; ' +
      '<b>' + articles.toLocaleString() + '</b> articles &middot; <b>' + bytes(size) + '</b>';

    els.results.textContent = '';
    if (!shown.length) {
      var empty = document.createElement('p');
      empty.className = 'empty';
      empty.textContent = 'No pack matches that search.';
      els.results.appendChild(empty);
      return;
    }
    if (!els.group.checked) {
      var flat = document.createElement('div');
      flat.className = 'packs';
      shown.forEach(function (pack) { flat.appendChild(packCard(pack, query)); });
      els.results.appendChild(flat);
      return;
    }
    data.collections.forEach(function (collection) {
      var members = shown.filter(function (pack) { return pack.collection === collection.id; });
      if (!members.length) return;
      var section = document.createElement('section');
      section.className = 'collection';
      var packBytes = members.reduce(function (sum, pack) { return sum + pack.bytes; }, 0);
      var packArticles = members.reduce(function (sum, pack) { return sum + pack.articles; }, 0);
      section.innerHTML = '<div class="collection-head"><h2>' + escapeHtml(collection.name) + '</h2>' +
        '<p>' + escapeHtml(collection.description) + '</p>' +
        '<div class="meta">' + members.length + ' packs &middot; ' +
          packArticles.toLocaleString() + ' articles &middot; ' + bytes(packBytes) + '</div></div>';
      var grid = document.createElement('div');
      grid.className = 'packs';
      members.forEach(function (pack) { grid.appendChild(packCard(pack, query)); });
      section.appendChild(grid);
      els.results.appendChild(section);
    });
  }

  var timer = null;
  els.q.addEventListener('input', function () {
    clearTimeout(timer);
    timer = setTimeout(render, 120);
  });
  ['collection', 'kind', 'sort', 'group'].forEach(function (name) {
    els[name].addEventListener('change', render);
  });
  document.addEventListener('keydown', function (event) {
    if (event.key === '/' && document.activeElement !== els.q) {
      event.preventDefault();
      els.q.focus();
    } else if (event.key === 'Escape' && document.activeElement === els.q) {
      els.q.value = '';
      render();
    }
  });
  function paintTheme() {
    els.theme.textContent = document.documentElement.dataset.theme === 'dark' ? 'Light mode' : 'Dark mode';
  }
  els.theme.addEventListener('click', function () {
    var next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
    document.documentElement.dataset.theme = next;
    try { localStorage.setItem('pocketwiki-theme', next); } catch (e) { /* storage unavailable */ }
    paintTheme();
  });

  els.total.textContent = bytes(__TOTAL_BYTES__);
  paintTheme();
  render();
})();
</script>
</body>
</html>
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--catalog", type=Path, default=DEFAULT_CATALOG)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    data = collect(args.catalog, args.manifest)
    args.output.write_text(render(data), encoding="utf-8")
    total = sum(pack["bytes"] for pack in data["packs"])
    print(f"Wrote {args.output} with {len(data['packs'])} packs, "
          f"{len(data['collections'])} collections, {total} bytes of packs "
          f"({args.output.stat().st_size} bytes of HTML)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
