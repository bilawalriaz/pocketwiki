#!/usr/bin/env python3
"""Audit articles for leftover generator commentary with a local LLM.

The deterministic rules in ``tools/article_text.py`` remove the commentary
shapes we have seen. This tool exists to answer the question those rules cannot
answer on their own: *what did they miss?* It sends articles to a local
OpenAI-compatible server and asks a small model to quote any text that is not
article content -- edit logs, word-count self-assessments, compliance notes,
and template scaffolding labels such as "Body" or "Overview".

Two design choices make the output trustworthy:

* **Quotes, not offsets.** A language model cannot count characters reliably,
  so asking for offsets produces confident nonsense. The model is asked for an
  exact verbatim quote instead, and this tool locates the quote itself. The
  offsets in the report are therefore verified by construction, and a quote
  that cannot be found is reported as ``verified: false`` -- a direct measure
  of how often the model invents spans.
* **Sampling, not trust.** Use ``--mode sample`` to audit articles the rules
  judged clean. Anything the model finds there is a recall gap in the rules;
  ``--suggest-rules`` writes those quotes out so they can be promoted into
  ``SELF_ASSESSMENT``.

Modes:
  candidates   audit only the articles the deterministic rules already flag
  sample       audit a random sample of articles the rules judge clean
  all          audit every article

Works against anything speaking the OpenAI chat-completions API. For LM Studio
(the default base URL) and for ``llama-server`` on a GPU box alike:

    # Prototype against LM Studio.
    python3 tools/audit_meta_commentary.py --mode candidates --limit 11

    # Recall check on a sample of clean articles.
    python3 tools/audit_meta_commentary.py --mode sample --limit 40 --repeat 2

    # Against llama.cpp on another machine (see --base-url).
    python3 tools/audit_meta_commentary.py \\
        --base-url http://aero:8080/v1 --model minicpm5-2b --mode all \\
        --workers 4 --report tools/meta-audit.jsonl

MiniCPM5-2B settings. The model card recommends ``temperature=1.0, top_p=0.95``
and, on llama.cpp, ``min_p=0.0`` (llama.cpp's 0.05 default causes repetition
loops). Those targets are for open-ended generation. This is an extraction
task, where the goal is one stable answer per article, so the default here is
greedy decoding with thinking disabled (``/no_think``) and ``--repeat`` to
prove the answers are stable. Pass ``--temperature`` to override.
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import hashlib
import json
import os
import random
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from article_text import strip_meta_commentary  # noqa: E402
from content_paths import ARTICLES  # noqa: E402

DEFAULT_BASE_URL = "http://localhost:1234/v1"
DEFAULT_MODEL = "minicpm5-2b"

SYSTEM_PROMPT = (
    "You audit encyclopedia articles for text left behind by the writing "
    "process. You report only text that is NOT article content: notes about "
    "editing, word counts or length limits, self-assessment, compliance "
    "checks, and template scaffolding labels such as 'Body' or 'Overview'. "
    "Copy each offending span EXACTLY as it appears in the input. Never "
    "paraphrase, never invent, and never report ordinary article prose. "
    "Reply with JSON only."
)

USER_TEMPLATE = """Audit this article. Report every span that is not article content.

A span is offending when it talks about the article itself, or about writing it,
rather than informing the reader. Offending categories:

- edit notes: text that lists edits that were made to the article;
- word-count or length self-assessment: text that judges the article against a
  length target or budget;
- self-assessment or compliance remarks: text that asserts the article met a
  rule or avoided some prohibited material;
- scaffolding: a heading that is a placeholder label rather than a subject.

Ordinary article prose is NOT offending, even when it mentions words, counts,
or change over time. Writing about a length target in the subject matter, or a
heading that names a real subject or a real historical change, is normal content.

Reply with JSON only:
{{"contaminated": true|false, "spans": [{{"quote": "<verbatim text>",
"kind": "edit-log|word-count|self-assessment|scaffolding|other",
"why": "<short reason>"}}]}}

Every quote must be copied exactly, character for character, from the article
below. Report nothing that is not in that article text. Use an empty spans list
when the article is clean.

<article>
{text}
</article>"""


SPAN_SCHEMA = {
    "name": "meta_commentary_audit",
    "strict": True,
    "schema": {
        "type": "object",
        "properties": {
            "contaminated": {"type": "boolean"},
            "spans": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "quote": {"type": "string"},
                        "kind": {"type": "string"},
                        "why": {"type": "string"},
                    },
                    "required": ["quote", "kind", "why"],
                },
            },
        },
        "required": ["contaminated", "spans"],
    },
}


class ServerError(RuntimeError):
    """A failed request, with the server's own message attached."""

    def __init__(self, status: int, body: str):
        super().__init__(f"HTTP {status}: {body.strip()[:300]}")
        self.status = status
        self.body = body


# Retried statuses. 400 is included on purpose: LM Studio reports a squeezed
# context as "Engine protocol predict stream returned an error: Context size has
# been exceeded" with HTTP 400, which clears as other in-flight requests finish.
RETRYABLE = {400, 408, 409, 425, 429, 500, 502, 503, 504}


def _post(url: str, payload: dict, timeout: int, retries: int = 3) -> dict:
    delay = 1.0
    attempt = 0
    while True:
        try:
            req = urllib.request.Request(
                url,
                data=json.dumps(payload).encode("utf-8"),
                headers={"Content-Type": "application/json"},
            )
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return json.load(resp)
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8", "replace")
            if exc.code not in RETRYABLE or attempt >= retries:
                raise ServerError(exc.code, body) from exc
        except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
            if attempt >= retries:
                raise ServerError(0, str(exc)) from exc
        attempt += 1
        time.sleep(delay + random.random() * 0.5)
        delay *= 2


def _extract_json(content: str) -> dict:
    """Parse the model's reply, tolerating prose or fences around the object."""
    content = content.strip()
    fence = re.search(r"```(?:json)?\s*(.*?)```", content, re.S)
    if fence:
        content = fence.group(1).strip()
    try:
        return json.loads(content)
    except json.JSONDecodeError:
        pass
    start = content.find("{")
    end = content.rfind("}")
    if start != -1 and end > start:
        return json.loads(content[start:end + 1])
    raise ValueError(f"no JSON object in reply: {content[:200]!r}")


def ask(client: "ChatClient", text: str) -> dict:
    """One audit call. Returns the parsed verdict plus the raw reply."""
    payload = {
        "model": client.model,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": USER_TEMPLATE.format(text=text)},
        ],
        "temperature": client.temperature,
    }
    if client.max_tokens:
        payload["max_tokens"] = client.max_tokens
    if client.reasoning_effort is not None:
        payload["reasoning_effort"] = client.reasoning_effort
    if client.chat_template_kwargs:
        payload["chat_template_kwargs"] = client.chat_template_kwargs
    if client.structured:
        payload["response_format"] = {"type": "json_schema", "json_schema": SPAN_SCHEMA}
    body = _post(client.base_url + "/chat/completions", payload, client.timeout, client.retries)
    message = body["choices"][0]["message"]
    verdict = _extract_json(message.get("content") or "")
    verdict["raw"] = message if client.keep_raw else None
    return verdict


def locate(text: str, quote: str) -> tuple[int, int] | None:
    """Find ``quote`` in ``text``, tolerating whitespace differences.

    Returns ``(start, end)`` in the original text, or ``None`` when the quote
    cannot be found, which means the model did not copy it faithfully.
    """
    quote = quote.strip()
    if not quote:
        return None
    start = text.find(quote)
    if start != -1:
        return start, start + len(quote)
    # Same text, different whitespace: locate it via a whitespace-tolerant scan.
    pattern = re.compile(r"\s+".join(re.escape(w) for w in quote.split()), re.S)
    match = pattern.search(text)
    if match:
        return match.start(), match.end()
    return None


class ChatClient:
    def __init__(self, base_url: str, model: str, temperature: float,
                 max_tokens: int | None, structured: bool, timeout: int,
                 keep_raw: bool, reasoning_effort: str | None = "none",
                 chat_template_kwargs: dict | None = None, retries: int = 3):
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.temperature = temperature
        self.max_tokens = max_tokens
        self.structured = structured
        self.timeout = timeout
        self.keep_raw = keep_raw
        self.reasoning_effort = reasoning_effort
        self.chat_template_kwargs = chat_template_kwargs or None
        self.retries = retries


def audit_file(client: ChatClient, path: Path, repeat: int) -> dict:
    """Audit one article, ``repeat`` times, and verify each quoted span."""
    text = path.read_text(encoding="utf-8")
    rule_verdict = bool(strip_meta_commentary(text)[1])
    runs = []
    for _ in range(repeat):
        try:
            runs.append(ask(client, text))
        except Exception as exc:  # a single failure must not kill the run
            runs.append({"error": f"{type(exc).__name__}: {exc}"})

    errors = [r["error"] for r in runs if "error" in r]
    ok_runs = [r for r in runs if "error" not in r]
    contaminated = [bool(r.get("contaminated")) for r in ok_runs]

    spans = []
    for run in ok_runs[:1]:
        for span in run.get("spans") or []:
            quote = (span.get("quote") or "").strip()
            found = locate(text, quote)
            spans.append({
                "quote": quote,
                "kind": span.get("kind"),
                "why": span.get("why"),
                "start": found[0] if found else None,
                "end": found[1] if found else None,
                "verified": bool(found),
            })

    return {
        "path": str(path),
        "sha1": hashlib.sha1(text.encode()).hexdigest(),
        "chars": len(text),
        "rule_flagged": rule_verdict,
        "model_contaminated": any(contaminated),
        "repeat_consistent": len(set(contaminated)) <= 1,
        "spans": spans,
        "verified_spans": sum(1 for s in spans if s["verified"]),
        "unverified_quotes": sum(1 for s in spans if not s["verified"]),
        "errors": errors,
        "raw": [r.get("raw") for r in ok_runs[:1]],
    }


def select(root: Path, mode: str, limit: int | None, seed: int) -> list[Path]:
    everything = sorted(root.rglob("*.md"))
    flagged = [p for p in everything if strip_meta_commentary(p.read_text())[1]]
    if mode == "candidates":
        pool = flagged
    elif mode == "sample":
        pool = [p for p in everything if p not in set(flagged)]
        random.Random(seed).shuffle(pool)
    else:
        pool = everything
    return pool[:limit] if limit else pool


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ARTICLES,
                        help="article root to audit (default: the content checkout)")
    parser.add_argument("--mode", choices=("candidates", "sample", "all"), default="candidates")
    parser.add_argument("--limit", type=int, help="audit at most this many articles")
    parser.add_argument("--seed", type=int, default=1, help="sampling seed for --mode sample")
    parser.add_argument("--base-url", default=os.environ.get("POCKETWIKI_LLM_URL", DEFAULT_BASE_URL),
                        help=f"OpenAI-compatible base URL (default: {DEFAULT_BASE_URL})")
    parser.add_argument("--model", default=os.environ.get("POCKETWIKI_LLM_MODEL", DEFAULT_MODEL))
    parser.add_argument("--temperature", type=float, default=0.0,
                        help="0 for greedy decoding, so verdicts are reproducible")
    parser.add_argument("--reasoning-effort", default="none",
                        help="'none' disables the thinking block (MiniCPM5 supports it; "
                             "measured 0 reasoning tokens vs 254, and ~5x faster). "
                             "Use 'default' to leave the server's setting alone.")
    parser.add_argument("--chat-template-kwargs",
                        help='JSON passed through to the chat template; llama.cpp needs '
                             '{"enable_thinking": false} for the same effect')
    parser.add_argument("--max-tokens", type=int, default=512)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--workers", type=int, default=2,
                        help="concurrent requests. The server must fit this many "
                             "prompts in its context at once, so keep it low unless you "
                             "raised the server's context length")
    parser.add_argument("--retries", type=int, default=4,
                        help="retries per request; the server rejects requests with "
                             "HTTP 400 when its context is momentarily full")
    parser.add_argument("--repeat", type=int, default=1,
                        help="run each article N times to test stability")
    parser.add_argument("--no-structured", dest="structured", action="store_false",
                        help="skip response_format for servers without JSON schema support")
    parser.add_argument("--report", type=Path, default=ROOT / "tools" / "meta-audit.jsonl",
                        help="append per-article results here, and resume from it")
    parser.add_argument("--suggest-rules", type=Path,
                        help="write quotes found in rule-clean articles, to promote into the rules")
    parser.add_argument("--keep-raw", action="store_true", help="store raw model replies in the report")
    args = parser.parse_args(argv)

    if not args.root.is_dir():
        print(f"article root not found: {args.root}", file=sys.stderr)
        return 2

    done: set[str] = set()
    if args.report.exists():
        for line in args.report.read_text().splitlines():
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            # Only successful rows count as done, so a rerun retries whatever
            # failed instead of silently treating it as audited.
            if row.get("sha1") and not row.get("errors"):
                done.add(row["sha1"])

    targets = select(args.root, args.mode, args.limit, args.seed)
    remaining = [p for p in targets
                 if hashlib.sha1(p.read_text(encoding="utf-8").encode()).hexdigest() not in done]

    # The same article can appear in several packs: 686 of the 6,257 checked-in
    # files are byte-identical copies. Audit each distinct text once and copy the
    # verdict to its copies, so a long run does not pay for them repeatedly and
    # cannot produce different verdicts for the same content.
    groups: dict[str, list[Path]] = {}
    for path in remaining:
        sha = hashlib.sha1(path.read_text(encoding="utf-8").encode()).hexdigest()
        groups.setdefault(sha, []).append(path)
    representatives = [paths[0] for paths in groups.values()]

    print(f"{args.mode}: {len(targets)} selected, {len(remaining)} to audit "
          f"({len(representatives)} distinct texts, {len(done)} already in {args.report}) "
          f"against {args.base_url} model={args.model} "
          f"temperature={args.temperature} repeat={args.repeat}",
          flush=True)
    if len(remaining) != len(representatives):
        print(f"  ({len(remaining) - len(representatives)} duplicate copies reuse a verdict)",
              flush=True)

    client = ChatClient(
        args.base_url, args.model, args.temperature, args.max_tokens,
        args.structured, args.timeout, args.keep_raw,
        reasoning_effort=None if args.reasoning_effort == "default" else args.reasoning_effort,
        chat_template_kwargs=json.loads(args.chat_template_kwargs) if args.chat_template_kwargs else None,
        retries=args.retries,
    )
    results = []
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with open(args.report, "a", encoding="utf-8") as sink, \
            concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(audit_file, client, p, args.repeat): p for p in representatives}
        for future in concurrent.futures.as_completed(futures):
            path = futures[future]
            try:
                result = future.result()
            except Exception as exc:
                result = {"path": str(path), "sha1": "", "errors": [str(exc)], "spans": []}
            copies = groups.get(result.get("sha1") or "", [path])
            for index, copy in enumerate(copies):
                row = dict(result)
                row["path"] = str(copy)
                if index:
                    row["duplicate_of"] = str(path)
                sink.write(json.dumps(row, ensure_ascii=False) + "\n")
                results.append(row)
            sink.flush()
            mark = "CONTAMINATED" if result.get("model_contaminated") else "clean"
            extra = f" (+{len(copies) - 1} copies)" if len(copies) > 1 else ""
            print(f"  [{len(results)}/{len(remaining)}] {mark:12s} "
                  f"{len(result.get('spans') or [])} spans  {path.name}{extra}", flush=True)

    # --- summary ----------------------------------------------------------
    model_hit = [r for r in results if r.get("model_contaminated")]
    rule_hit = [r for r in results if r.get("rule_flagged")]
    unverified = sum(r.get("unverified_quotes", 0) for r in results)
    errored = [r for r in results if r.get("errors")]
    unstable = [r for r in results if r.get("repeat_consistent") is False]
    print()
    print(f"audited: {len(results)}   model flagged: {len(model_hit)}   "
          f"rules flagged: {len(rule_hit)}   errors: {len(errored)}")
    print(f"quotes that could not be located verbatim: {unverified}")
    if args.repeat > 1:
        print(f"articles whose verdict changed across {args.repeat} runs: {len(unstable)}")
    if errored:
        kinds = collections.Counter(
            re.sub(r"\d+", "N", r["errors"][0])[:120] for r in errored
        )
        print("failures by type:")
        for message, count in kinds.most_common(4):
            print(f"  {count:4d} x {message}")
        if any("Context size has been exceeded" in (r["errors"][0] or "") for r in errored):
            print(
                "\nThose articles did not fit the server's context, which is a server\n"
                "setting, not a transient error. Context is divided across parallel slots:\n"
                "LM Studio loaded 8192 tokens with parallel=4, i.e. 2048 tokens per request,\n"
                "which rejects articles over roughly 8k characters. Raise Context Length in\n"
                "the model's load settings (MiniCPM5-2B supports 131072), or pass a larger\n"
                "-c with --parallel on llama.cpp. Rerun afterwards: failed rows are retried."
            )

    novel = []
    for r in results:
        if r.get("model_contaminated") and not r.get("rule_flagged"):
            for span in r.get("spans") or []:
                if span.get("verified"):
                    novel.append((r["path"], span))
    if novel:
        print(f"\nRECALL GAP: {len(novel)} verified span(s) in articles the rules judged clean:")
        for path, span in novel[:20]:
            print(f"  {Path(path).name}: {span['quote'][:120]!r}")
    elif args.mode == "sample":
        print("\nNo recall gap found in this sample (verified quotes only).")

    if args.suggest_rules and novel:
        args.suggest_rules.parent.mkdir(parents=True, exist_ok=True)
        args.suggest_rules.write_text(
            json.dumps([{"path": p, "quote": s["quote"], "kind": s["kind"]}
                        for p, s in novel], indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8")
        print(f"wrote {len(novel)} candidate pattern(s) to {args.suggest_rules}")

    return 1 if errored else 0


if __name__ == "__main__":
    raise SystemExit(main())
