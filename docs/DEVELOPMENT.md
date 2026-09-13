# Development

This guide describes the normal local workflow. Do not commit generated
firmware, archives, downloaded content, `sdkconfig`, or managed components.

## Requirements

- Python 3.10 or later
- `pytest` for host tests
- PlatformIO with `espressif32@7.0.1`, or ESP-IDF 6.0.1
- Swift 5.10 or later for the optional desktop flasher

## Test the host tools

```sh
python3 -m pytest -q
```

The suite checks archive validation, deterministic pack output, sanitization,
link rewriting, title-normalization parity, QR symbol encoding, and
reference-server routes.

## Build firmware

PlatformIO prepares the stylesheet, catalogue, and built-in gzip pack before
each build. It then stages flash-ready files in `firmware/dist/<environment>/`.

```sh
# 4 MB ESP32-C3
pio run -d firmware -e esp32-c3

# ESP32-S3 (the repository's 16 MB deployment model)
pio run -d firmware -e esp32-s3
```

The current PlatformIO board declaration reports an 8 MB, no-PSRAM
DevKitC-1. The 16 MB S3 layout is the intended deployment model, but must be
validated on the actual module before flashing. The pack-system benchmark
evidence is summarized in [the final recommendation](FINAL_RECOMMENDATION.md);
the live C3 serial measurements are in [DEVICE_BENCHMARK.md](DEVICE_BENCHMARK.md).

You can also use ESP-IDF:

```sh
source ~/esp/esp-idf/export.sh
idf.py -C firmware set-target esp32c3  # Use esp32s3 for the S3.
idf.py -C firmware build
```

The built-in archive comes from `articles/db-packs/biology-health/` in the
`pocketwiki-content` checkout, found at `../pocketwiki-content` unless
`POCKETWIKI_CONTENT_DIR` points elsewhere. To test a
different source directory with ESP-IDF, pass an absolute path:

```sh
idf.py -C firmware -DPW_ARTICLES_DIR=/absolute/path/to/articles build
```

## Flash a board

Build first. The arguments must match the board and its partition table.

```sh
# ESP32-C3
python3 tools/flash_all.py --port /dev/ttyACM0 \
  --fw-dir firmware/dist/esp32-c3

# ESP32-S3
python3 tools/flash_all.py --port /dev/cu.usbmodemXXXX --chip esp32s3 \
  --partitions firmware/dist/esp32-s3/partitions.csv \
  --fw-dir firmware/dist/esp32-s3
```

The script checks that each image fits its partition before it calls esptool.
Use `--no-firmware` to write only content and index. Use `--no-content` to
write only the bootloader, partition table, and application.

For a fresh ESP32-S3 setup, erase the complete flash before running the normal
flash command. This intentionally removes optional packs and saved Wi-Fi
credentials:

```sh
python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX erase-flash
python3 tools/flash_all.py --port /dev/cu.usbmodemXXXX --chip esp32s3 \
  --partitions firmware/dist/esp32-s3/partitions.csv \
  --fw-dir firmware/dist/esp32-s3
```

## Preview the web interface

```sh
python3 tools/pack_content.py build ../pocketwiki-content/articles/db-packs/biology-health build/preview --codec gzip
python3 tools/reference_server.py build/preview --port 8080
```

The reference server helps you check routes and layout. It does not model
device memory, BLE, flash latency, or interrupted uploads.

The public landing page is [`pocketwiki3.html`](../pocketwiki3.html), the
BoardUI edition of the project overview. It is staged at both `/pocketwiki/`
(the canonical URL) and `/pocketwiki3/`; [`index.html`](../index.html) is the
earlier design and is only staged at `/pocketwiki2/`. The device's reader and
manager are separate local pages served by the firmware. When changing web copy,
update the firmware templates and reference server in the same change, then run
the host tests and preview both reader widths. To publish the landing page, run
`sh cloudflare/site/prepare.sh` followed by
`wrangler deploy --config cloudflare/site/wrangler.jsonc`.

The experimental PWPK browser path can be exercised with
`tools/pwpk_browser_server.py` and `/benchmark`; see
[BROWSER_BENCHMARK.md](BROWSER_BENCHMARK.md). It is separate from the
production built-in archive.

## Article text and its invariants

Article text is not in this repository. It lives in the companion
`pocketwiki-content` checkout and is licensed CC BY-SA 4.0. The generators in
`tools/` read that checkout and write Markdown into it.

`tools/article_text.py` is the single definition of a finalized article:

- no trailing generator commentary (edit logs, word-count self-assessments),
- exactly one `h1`, with later `h1` headings demoted to `h2`,
- a CC BY-SA 4.0 footer naming the source article.

Every generator calls `finalize_article` before writing, so regenerating content
cannot lose those properties.

```sh
python3 tools/finalize_articles.py            # report drift (dry run)
python3 tools/finalize_articles.py --check    # exit 1 if any article has drifted
python3 tools/finalize_articles.py --apply    # repair, with a JSON report
```

The commentary rules are deliberately narrow. `## What Changed for Society` is
legitimate content in an article about social change, so only a bolded
`**Changes made:**`-style header, or a self-assessment phrase inside the last
few lines, counts as generator commentary.

### Auditing the rules' recall

`tools/audit_meta_commentary.py` asks a local model to quote any remaining
non-article text, and verifies every quote verbatim against the file. Run it on
the flagged set to confirm the rules are right, and on a sample of articles the
rules judged clean to bound what they miss.

```sh
# Confirmation: only articles the rules already flag.
python3 tools/audit_meta_commentary.py --mode candidates

# Recall: a random sample the rules judged clean.
python3 tools/audit_meta_commentary.py --mode sample --limit 300 --repeat 2
```

It speaks the OpenAI chat-completions API, so the same command drives LM Studio
on a laptop and `llama-server` on a GPU box:

```sh
python3 tools/audit_meta_commentary.py --base-url http://aero:8080/v1 \
    --model minicpm5-2b --mode all --workers 4 --report tools/meta-audit.jsonl
```

Verification is the point: a quote that cannot be found in the article is
reported as unverified, which is how you measure the model inventing spans, and
`--repeat 2` reports verdicts that changed between runs.

MiniCPM5-2B is a hybrid-reasoning model whose thinking block is on by default
and will spend the entire token budget before answering. The tool therefore
sends `reasoning_effort: "none"` (measured: 0 reasoning tokens instead of 254,
about 5x faster). On llama.cpp, pass
`--chat-template-kwargs '{"enable_thinking": false}'` for the same effect. The
model card's `temperature=1.0, top_p=0.95, min_p=0.0` are generation settings;
this is an extraction task, so greedy decoding is the default here.

### Cleaning the wiki-distill database

The content repository is generated from
`~/wiki-distill/educational-source.db`, and `minimax_distillations.draft` is the
column every pack generator reads. Clearing generator commentary there fixes
every pack at once instead of per exported file:

```sh
python3 tools/clean_distill_db.py            # report only, writes nothing
python3 tools/clean_distill_db.py --apply    # back up, then write
python3 tools/clean_distill_db.py --verify   # prove a further pass is a no-op
```

It writes a backup beside the database before modifying anything, applies every
change in one transaction, refuses a removal larger than
`--max-removal-fraction` (35% by default) or one that would leave an
implausibly short draft, and records each change in `cleanup_audit` under
`meta-cleanup-deterministic-v2`, matching the earlier `meta-cleanup-*` passes.
It is idempotent.

`generated_draft` is deliberately left alone: it is the raw pre-trim model
output, kept for provenance, and nothing in this repository reads it.

To check a whole corpus, auditing only the trailing 2,000 characters of each
draft is enough, because contamination from this pipeline is always a trailing
postamble -- that held for every removal in the database. Trailing text runs
roughly eight times faster than whole articles.

When a model finds commentary that no rule covers yet, remove it by the verified
span rather than by a pattern:

```sh
python3 tools/apply_audit_removals.py --audit build/meta-confirm.jsonl          # review
python3 tools/apply_audit_removals.py --audit build/meta-confirm.jsonl --apply \
    --exclude 11801,12630
```

It deletes the block of lines containing each verified quote, merges overlapping
blocks, and refuses any block that contains a markdown heading or that exceeds
the size cap. `--exclude` names model flags that review showed are article
content. This is the fallback for what rules cannot safely express: a rule
matching "changes made" also matched "Two changes made the katana dominant".

Every pass records its `original_text` in `cleanup_audit`, so a pass can be
rolled back from the audit trail rather than only from a backup.

### Outcome on the wiki-distill corpus (2026-09-13)

22,033 drafts, audited end to end on full text with MiniCPM5-2B:

- 157 drafts had generator commentary removed: 107 by rules and 51 by verified
  quote, overlapping on one draft.
- 231 glued headings were split across 213 drafts by
  `tools/fix_glued_headings.py`.
- The full-text sweep flagged 17 drafts and **none was commentary**. Reading each
  in context, 16 are article prose the model misread and one was a glued heading
  the splitter had already fixed. Examples worth remembering: "Contamination
  episode" is the 1989 eosinophilia-myalgia outbreak, "In editing, outcomes are
  coded as gains or losses" is the editing phase of prospect theory, and
  "## A note on practice" is a section about feature counts.
- The model's false-positive modes on this corpus, all seen repeatedly: a
  quantity followed by "words" ("30 million words of manuscript"), a heading
  containing an editorial word ("The Editorial Afterlife", "Revision history"),
  and CamelCase or hyphenated compounds.

The lesson for anyone extending the rules: add a pattern only after reading every
match it produces across the whole corpus. Three rounds of that discipline
removed rules that had deleted whole sections, and the sweep above is what
confirmed the remaining flags are content rather than a backlog.

## Before you open a pull request

1. Run `python3 -m pytest -q`.
2. Build firmware if you changed `firmware/`, CMake, Kconfig, or partitions.
3. Preview HTML or CSS changes at desktop and phone widths.
4. Keep archive readers and writers compatible, or change the format version.
5. State the user-visible change and the checks you ran.
