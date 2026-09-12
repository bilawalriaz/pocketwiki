#!/usr/bin/env python3
"""Build an offline optimisation baseline and compression comparison report.

This tool never publishes, refreshes the Android fallback catalogue, or writes
release files. It builds into a temporary directory and writes only the JSON
and Markdown report requested by ``--output`` (under the ignored ``build/``
tree by default).

The matrix intentionally measures complete portable packs, not just article
frames. A profile is only useful when its dictionary, index, wrapper, and FAT
cluster rounding are included in the accounting.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
from content_paths import ARTICLES, CATALOG, SAMPLE
sys.path.insert(0, str(ROOT / "tools"))

import archive_format as af  # noqa: E402
import pack_content  # noqa: E402

MAX_PACK_BYTES = 2 * 1024 * 1024
FAT_CLUSTER_BYTES = 4096
UPLOAD_RESERVE_BYTES = 8192
DICT_OPTIONS = (0, 4096, 8192, 16384, 32768)
LEVEL_OPTIONS = (9, 15, 19)
WINDOW_OPTIONS = (12, 13, 14)


def command_version(command: str) -> str | None:
    exe = shutil.which(command)
    if not exe:
        return None
    try:
        return subprocess.run([exe, "--version"], capture_output=True,
                              text=True, check=False).stdout.strip().splitlines()[0]
    except (OSError, IndexError):
        return None


def sha256_files(paths: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(str(path.relative_to(ROOT)).encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()


def source_hash(directory: Path) -> str:
    files = [p for p in directory.rglob("*")
             if p.is_file() and p.suffix.lower() in {".md", ".html", ".htm"}]
    return sha256_files(files)


def cluster_bytes(size: int) -> int:
    return math.ceil(size / FAT_CLUSTER_BYTES) * FAT_CLUSTER_BYTES


def copy_source(source_dir: Path, names: list[str] | None, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    if names is None:
        paths = [p for p in source_dir.rglob("*")
                 if p.is_file() and p.suffix.lower() in {".md", ".html", ".htm"}]
        for path in paths:
            relative = path.relative_to(source_dir)
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        return
    for name in names:
        path = source_dir / name
        if not path.is_file():
            raise FileNotFoundError(path)
        shutil.copy2(path, destination / path.name)


def build_one(spec: dict, profile: dict, workspace: Path) -> dict:
    source_dir = ARTICLES / spec["dir"]
    article_dir = workspace / "articles"
    output_dir = workspace / "archive"
    copy_source(source_dir, spec.get("articles"), article_dir)
    started = time.perf_counter()
    manifest = pack_content.build(
        str(article_dir), str(output_dir),
        codec=profile["codec"],
        dict_bytes_max=profile.get("dict_bytes", pack_content.ZSTD_DICT_MAX),
        zstd_level=profile.get("level", pack_content.ZSTD_LEVEL),
        zstd_window_log=profile.get("window_log", pack_content.ZSTD_WINDOW_LOG),
    )
    build_seconds = time.perf_counter() - started
    content = (output_dir / "content.bin").read_bytes()
    index = (output_dir / "index.bin").read_bytes()
    archive = af.Archive(content, index)
    portable = af.pack_pack_file(content, index)
    frames = [content[archive.payload_offset + e["content_offset"]:
                      archive.payload_offset + e["content_offset"] + e["comp_len"]]
              for e in archive.entries]
    windows = [af.zstandard.get_frame_parameters(frame).window_size for frame in frames] \
        if archive.format_version == af.FORMAT_VERSION else []
    title_bytes = len(index) - archive.strings_off
    result = {
        "source": spec["id"],
        "article_count": archive.count,
        "unique_source_title_identities": len({e["norm"] for e in archive.entries}),
        "sanitized_html_bytes": sum(e["uncomp_len"] for e in archive.entries),
        "compressed_frame_bytes": sum(e["comp_len"] for e in archive.entries),
        "dictionary_bytes": archive.dict_len,
        "fixed_index_bytes": af.INDEX_HEADER_SIZE + archive.count * af.ENTRY_SIZE,
        "title_string_bytes": title_bytes,
        "content_file_bytes": len(content),
        "index_file_bytes": len(index),
        "wrapper_header_bytes": af.PACK_HEADER_SIZE,
        "pwp_bytes": len(portable),
        "pwp_fat_allocated_bytes": cluster_bytes(len(portable)),
        "max_frame_window_bytes": max(windows, default=0),
        "max_compressed_article_bytes": max((e["comp_len"] for e in archive.entries), default=0),
        "max_uncompressed_article_bytes": max((e["uncomp_len"] for e in archive.entries), default=0),
        "build_seconds": round(build_seconds, 4),
        "manifest": manifest,
    }
    return result


def profile_name(profile: dict) -> str:
    if profile["codec"] == "gzip":
        return "gzip-9"
    return f"zstd-{profile['level']}-d{profile['dict_bytes']}-w{profile['window_log']}"


def profiles() -> list[dict]:
    result = [{"codec": "gzip"}]
    for level in LEVEL_OPTIONS:
        for dict_bytes in DICT_OPTIONS:
            for window_log in WINDOW_OPTIONS:
                result.append({"codec": "zstd", "level": level,
                               "dict_bytes": dict_bytes,
                               "window_log": window_log})
    return result


def catalog_specs(source: dict) -> list[dict]:
    return [{"id": p["id"], "dir": p.get("dir", "starter"),
             "articles": p["articles"]} for p in source["packs"]]


def aggregate(rows: list[dict]) -> dict:
    numeric = ("article_count", "sanitized_html_bytes", "compressed_frame_bytes",
               "dictionary_bytes", "fixed_index_bytes", "title_string_bytes",
               "content_file_bytes", "index_file_bytes", "pwp_bytes",
               "pwp_fat_allocated_bytes", "build_seconds")
    return {key: sum(row[key] for row in rows) for key in numeric} | {
        "pack_count": len(rows),
        "max_frame_window_bytes": max((r["max_frame_window_bytes"] for r in rows), default=0),
        "max_compressed_article_bytes": max((r["max_compressed_article_bytes"] for r in rows), default=0),
        "max_uncompressed_article_bytes": max((r["max_uncompressed_article_bytes"] for r in rows), default=0),
        "over_upload_limit_packs": sum(r["pwp_bytes"] > MAX_PACK_BYTES for r in rows),
    }


def collect_profile(profile: dict, specs: list[dict], work_root: Path) -> dict:
    rows = []
    for spec in specs:
        with tempfile.TemporaryDirectory(prefix="pocketwiki-bench-", dir=work_root) as tmp:
            rows.append(build_one(spec, profile, Path(tmp)))
    return {"name": profile_name(profile), "profile": profile,
            "aggregate": aggregate(rows), "packs": rows}


def write_markdown(report: dict, path: Path) -> None:
    lines = ["# PocketWiki optimisation benchmark", "", f"Generated: `{report['generated_at']}`", "",
             "This is an offline host report. It does not measure ESP32 latency, heap, or FAT "
             "metadata consumption on hardware.", "", "## Environment", ""]
    for key, value in report["environment"].items():
        lines.append(f"- `{key}`: `{value}`")
    lines += ["", "## Capacity model", "",
              "Pack storage is modeled with 4096-byte FAT allocation units and the firmware's "
              f"{UPLOAD_RESERVE_BYTES}-byte upload reserve. FAT/wear-levelling metadata is device "
              "reported and is not guessed from the raw partition size here.", "", "| partition | raw bytes | raw minus reserve |", "|---|---:|---:|"]
    for csv_name, parts in report["capacity"]["partitions"].items():
        for name, part in parts.items():
            lines.append(f"| {csv_name}:{name} | {part['size_bytes']:,} | {part['raw_minus_reserve_bytes']:,} |")
    lines += ["", "## Profile comparison", "", "| profile | articles | complete PWP bytes | FAT-allocated bytes | max zstd window | over 2 MiB |", "|---|---:|---:|---:|---:|---:|"]
    for item in report["profiles"]:
        a = item["aggregate"]
        lines.append(f"| {item['name']} | {a['article_count']:,} | {a['pwp_bytes']:,} | {a['pwp_fat_allocated_bytes']:,} | {a['max_frame_window_bytes']:,} | {a['over_upload_limit_packs']} |")
    lines += ["", "## Selection", "", f"Representative winning profile by complete bytes: `{report['selection']['representative_winner']}`.",
              f"Full-catalog profiles measured: {', '.join('`' + x + '`' for x in report['selection']['full_catalog_profiles'])}.", ""]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "optimization-benchmark")
    parser.add_argument("--catalog-limit", type=int, default=4,
                        help="representative catalogue packs for the matrix (default: 4)")
    parser.add_argument("--top-profiles", type=int, default=5,
                        help="number of representative winners to expand to the full catalogue")
    args = parser.parse_args(argv)

    source_path = CATALOG
    source = json.loads(source_path.read_text(encoding="utf-8"))
    builtin = {"id": "builtin-biology-health", "dir": "db-packs/biology-health"}
    representative = [
        {"id": "sample", "dir": "sample"},
        builtin,
        *catalog_specs(source)[:max(0, args.catalog_limit)],
    ]
    all_catalog = catalog_specs(source)
    selected_profiles = profiles()
    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pocketwiki-benchmark-") as tmp:
        work_root = Path(tmp)
        representative_results = [collect_profile(p, representative, work_root)
                                  for p in selected_profiles]
        ranked = sorted(representative_results,
                        key=lambda x: x["aggregate"]["pwp_bytes"])
        full_profiles = ["gzip-9"] + [x["name"] for x in ranked[:args.top_profiles]]
        by_name = {profile_name(p): p for p in selected_profiles}
        full_results = [collect_profile(by_name[name], all_catalog, work_root)
                        for name in full_profiles]

    versions = {
        "python": sys.version.split()[0],
        "platform": platform.platform(),
        "zstandard_python": pack_content.zstandard.__version__,
        "zstd_vendored": "1.5.7",
        "platformio": command_version("pio"),
        "idf": command_version("idf.py"),
        "cc": command_version("cc"),
    }
    partitions = {}
    for csv in (ROOT / "firmware" / "partitions.csv", ROOT / "firmware" / "partitions_16mb.csv"):
        parts = af.parse_partitions_csv(str(csv))
        partitions[csv.name] = {name: {
            "offset_bytes": value["offset"],
            "size_bytes": value["size"],
            "raw_minus_reserve_bytes": max(0, value["size"] - UPLOAD_RESERVE_BYTES),
            "cluster_bytes": FAT_CLUSTER_BYTES if name == "packs" else None,
        } for name, value in parts.items() if name in {"content", "index", "packs"}}
    environment = {
        "git_revision": subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                        capture_output=True, text=True, check=True).stdout.strip(),
        "git_status": subprocess.run(["git", "status", "--short"], cwd=ROOT,
                                      capture_output=True, text=True, check=True).stdout.strip() or "clean",
        **versions,
        "catalog_source_sha256": hashlib.sha256(source_path.read_bytes()).hexdigest(),
        "biology_source_sha256": source_hash(ARTICLES / "db-packs" / "biology-health"),
        "starter_source_sha256": source_hash(ARTICLES / "starter"),
        "sample_source_sha256": source_hash(SAMPLE),
        "partitions_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in (ROOT / "firmware" / "partitions.csv",
                                        ROOT / "firmware" / "partitions_16mb.csv")},
    }
    representative_winner = ranked[0]["name"]
    report = {
        "schema": 1,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "environment": environment,
        "constants": {"max_upload_bytes": MAX_PACK_BYTES,
                       "fat_cluster_bytes": FAT_CLUSTER_BYTES,
                       "upload_reserve_bytes": UPLOAD_RESERVE_BYTES},
        "capacity": {"partitions": partitions},
        "profiles": full_results,
        "representative_profiles": representative_results,
        "selection": {"representative_winner": representative_winner,
                       "full_catalog_profiles": full_profiles,
                       "representative_sources": [s["id"] for s in representative]},
    }
    json_path = output / "report.json"
    json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_markdown(report, output / "report.md")
    print(json.dumps({"output": str(output), "representative_winner": representative_winner,
                      "full_catalog_profiles": full_profiles}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
