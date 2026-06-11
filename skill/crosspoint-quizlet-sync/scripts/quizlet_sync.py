#!/usr/bin/env python3
"""Build Quizlet-compatible flashcard TXT files and route them to CrossPoint decks."""

from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple


Pair = Tuple[str, str]


@dataclass(frozen=True)
class DeckConfig:
    key: str
    label: str
    folder: str
    source_lang: str
    default_target_lang: str


@dataclass
class DeckResult:
    deck: str
    local_path: str
    remote_path: str
    cards: int
    uploaded: bool
    web_uploaded: bool = False
    web_path: str = ""


@dataclass
class SyncSummary:
    results: List[DeckResult] = field(default_factory=list)
    skipped: int = 0
    translated_count: int = 0
    translation_errors: int = 0
    ambiguous_lines: int = 0
    unrouted_lines: int = 0


DECKS: Dict[str, DeckConfig] = {
    "german": DeckConfig("german", "German", "german", "de", "en"),
    "ukrainian": DeckConfig("ukrainian", "Ukrainian", "ukrainian", "uk", "en"),
    "english": DeckConfig("english", "English", "english", "en", "de"),
}

GERMAN_WORDS = {
    "aber",
    "als",
    "auch",
    "auf",
    "bei",
    "bis",
    "das",
    "dem",
    "den",
    "der",
    "des",
    "die",
    "doch",
    "durch",
    "ein",
    "eine",
    "einem",
    "einen",
    "einer",
    "für",
    "gegen",
    "ich",
    "ihr",
    "im",
    "ist",
    "mit",
    "nach",
    "nicht",
    "schon",
    "sich",
    "und",
    "vom",
    "von",
    "zum",
    "zur",
    "über",
}

ENGLISH_WORDS = {
    "a",
    "an",
    "and",
    "are",
    "at",
    "be",
    "for",
    "from",
    "have",
    "how",
    "in",
    "is",
    "it",
    "look",
    "of",
    "on",
    "or",
    "that",
    "the",
    "their",
    "there",
    "they",
    "this",
    "to",
    "up",
    "was",
    "we",
    "with",
    "you",
    "your",
}

GERMAN_SUFFIXES = ("chen", "keit", "heit", "lich", "schaft", "tion", "ung", "erweise")
ENGLISH_SUFFIXES = ("ed", "er", "est", "ing", "less", "ly", "ment", "ness", "ship", "tion")
GERMAN_FRAGMENTS = ("sch", "tsch", "ä", "ö", "ü", "ß")
ENGLISH_FRAGMENTS = ("ough", "tion", "tional", "sh", "th", "wh")

CYRILLIC_RE = re.compile(r"[\u0400-\u04FF]")
LATIN_TOKEN_RE = re.compile(r"[A-Za-zÄÖÜäöüß]+")
DECK_HINT_PREFIX = "[[deck:"


def _parse_input_line(raw: str) -> Tuple[str, str | None, str | None] | None:
    line = raw.strip()
    if not line:
        return None

    deck_hint = None
    if line.startswith(DECK_HINT_PREFIX):
        hint_end = line.find("]]")
        if hint_end != -1:
            candidate = line[len(DECK_HINT_PREFIX) : hint_end].strip().lower()
            if candidate in DECKS:
                deck_hint = candidate
            line = line[hint_end + 2 :].lstrip()

    if not line or line.startswith("#"):
        return None

    if "\t" in line:
        left, right = line.split("\t", 1)
    elif "::" in line:
        left, right = line.split("::", 1)
    elif "," in line:
        left, right = line.split(",", 1)
    else:
        return line, None, deck_hint

    term = left.strip()
    translation = right.strip()
    if not term or not translation:
        return None
    return term, translation, deck_hint


def _translate_mymemory(text: str, source_lang: str, target_lang: str, timeout_sec: int) -> str:
    query = urllib.parse.urlencode({"q": text, "langpair": f"{source_lang}|{target_lang}"})
    url = f"https://api.mymemory.translated.net/get?{query}"
    request = urllib.request.Request(url, headers={"User-Agent": "crosspoint-quizlet-sync/2.0"})

    last_error: Exception | None = None
    for attempt in range(3):
        try:
            with urllib.request.urlopen(request, timeout=timeout_sec) as response:
                payload = response.read().decode("utf-8", errors="replace")
            data = json.loads(payload)
            translated = data.get("responseData", {}).get("translatedText", "")
            translated = html.unescape(str(translated)).strip()
            if translated:
                return translated
            raise RuntimeError("Empty translation response")
        except Exception as exc:
            last_error = exc
            if attempt < 2:
                time.sleep(0.5 * (attempt + 1))
                continue
            break

    raise RuntimeError(f"Translation failed for '{text}': {last_error}")


def _sanitize_prefix(prefix: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9._-]+", "-", prefix.strip())
    safe = safe.strip("._-")
    return safe or "flashcards"


def _build_output_path(output_dir: str, prefix: str, date_tag: str) -> str:
    base_name = f"{_sanitize_prefix(prefix)}-{date_tag}"
    candidate = os.path.join(output_dir, f"{base_name}.txt")
    if not os.path.exists(candidate):
        return candidate

    suffix = 2
    while True:
        candidate = os.path.join(output_dir, f"{base_name}-{suffix}.txt")
        if not os.path.exists(candidate):
            return candidate
        suffix += 1


def _write_quizlet_file(path: str, pairs: Sequence[Pair]) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        for term, translation in pairs:
            handle.write(f"{term}\t{translation}\n")


def _http_post_form(url: str, fields: Sequence[Tuple[str, str]], timeout_sec: int) -> Tuple[int, str]:
    body = urllib.parse.urlencode(fields).encode("utf-8")
    request = urllib.request.Request(
        url=url,
        data=body,
        method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_sec) as response:
            return response.getcode(), response.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        payload = exc.read().decode("utf-8", errors="replace") if exc.fp else str(exc)
        return exc.code, payload


def _ensure_remote_path(host: str, remote_path: str, timeout_sec: int) -> None:
    path = remote_path.strip()
    if not path or path == "/":
        return
    if not path.startswith("/"):
        path = "/" + path

    segments = [seg for seg in path.split("/") if seg]
    parent = "/"
    for segment in segments:
        status, payload = _http_post_form(
            f"{host}/mkdir",
            fields=[("name", segment), ("path", parent)],
            timeout_sec=timeout_sec,
        )
        payload_lower = payload.lower()
        if status != 200 and "already exists" not in payload_lower:
            raise RuntimeError(f"mkdir failed for '{segment}' under '{parent}': HTTP {status} {payload.strip()}")

        parent = "/" + segment if parent == "/" else parent + "/" + segment


def _build_multipart_payload(file_name: str, content: bytes) -> Tuple[str, bytes]:
    boundary = f"----crosspoint-{uuid.uuid4().hex}"
    chunks: List[bytes] = []
    chunks.append(
        (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="file"; filename="{file_name}"\r\n'
            "Content-Type: text/plain; charset=utf-8\r\n\r\n"
        ).encode("utf-8")
    )
    chunks.append(content)
    chunks.append(b"\r\n")
    chunks.append(f"--{boundary}--\r\n".encode("utf-8"))
    payload = b"".join(chunks)
    return boundary, payload


def _upload_file(host: str, remote_path: str, local_path: str, timeout_sec: int) -> Tuple[int, str]:
    with open(local_path, "rb") as handle:
        content = handle.read()

    boundary, payload = _build_multipart_payload(os.path.basename(local_path), content)
    query = urllib.parse.urlencode({"path": remote_path})
    url = f"{host}/upload?{query}"
    request = urllib.request.Request(
        url=url,
        data=payload,
        method="POST",
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(payload)),
        },
    )
    with urllib.request.urlopen(request, timeout=timeout_sec) as response:
        return response.getcode(), response.read().decode("utf-8", errors="replace")


DEFAULT_SYNC_AGENT = os.environ.get(
    "CROSSPOINT_SYNC_AGENT",
    # default: web/agent/sync_agent.py relative to this repo checkout
    str(Path(__file__).resolve().parents[3] / "web" / "agent" / "sync_agent.py"),
)


def run_device_web_sync(device_host: str, web_host: str, web_token: str, agent_path: str) -> bool:
    """Run one device<->web sync pass (files both ways + SM-2++ progress merge).

    A sleeping/offline device is not an error: the agent reports it and exits 0.
    """
    if not web_host:
        print("[warn] Skipping device<->web sync: no web host configured (CROSSPOINT_WEB).", file=sys.stderr)
        return False
    if not os.path.exists(agent_path):
        print(f"[warn] Skipping device<->web sync: agent not found at {agent_path}.", file=sys.stderr)
        return False

    cmd = [sys.executable, agent_path, "--device", device_host, "--web", web_host]
    if web_token:
        cmd += ["--token", web_token]
    print("[ok] Running device<->web sync pass...", flush=True)
    return subprocess.run(cmd).returncode == 0


def _upload_to_web(web_host: str, web_token: str, deck_key: str, local_path: str, timeout_sec: int) -> str:
    """PUT the deck file to the flashcards web app. Returns the web file path."""
    file_name = os.path.basename(local_path)
    with open(local_path, "rb") as handle:
        content = handle.read()

    url = f"{web_host}/api/decks/{deck_key}/files/{urllib.parse.quote(file_name)}"
    headers = {"Content-Type": "text/plain; charset=utf-8"}
    if web_token:
        headers["Authorization"] = f"Bearer {web_token}"
    request = urllib.request.Request(url=url, data=content, method="PUT", headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=timeout_sec) as response:
            status = response.getcode()
            payload = response.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status = exc.code
        payload = exc.read().decode("utf-8", errors="replace") if exc.fp else str(exc)
    if not 200 <= status < 300:
        raise RuntimeError(f"web upload failed for {url}: HTTP {status} {payload.strip()}")
    return f"/api/decks/{deck_key}/files/{file_name}"


def _normalize_host(host: str) -> str:
    value = host.strip()
    if not value:
        raise ValueError("Host cannot be empty")
    if not value.startswith("http://") and not value.startswith("https://"):
        value = "http://" + value
    return value.rstrip("/")


def _normalize_path(path: str) -> str:
    value = path.strip()
    if not value:
        return "/flashcards"
    if not value.startswith("/"):
        value = "/" + value
    if value.startswith("/~/flashcards"):
        value = "/flashcards" + value[len("/~/flashcards") :]
    if len(value) > 1 and value.endswith("/"):
        value = value[:-1]
    return value


def _load_input_lines(pairs_file: str | None) -> List[str]:
    if pairs_file:
        with open(pairs_file, "r", encoding="utf-8") as handle:
            lines = handle.readlines()
        suffix = Path(pairs_file).suffix.lower()
        if suffix in {".md", ".markdown"}:
            extracted = _extract_markdown_pair_lines(lines)
            if extracted:
                return extracted
        return lines
    return sys.stdin.readlines()


def _extract_markdown_pair_lines(lines: Sequence[str]) -> List[str]:
    extracted: List[str] = []
    in_fence = False
    current_deck: str | None = None

    for raw in lines:
        stripped = raw.strip()
        if stripped.startswith("## "):
            heading = stripped[3:].strip().lower()
            current_deck = next((key for key, deck in DECKS.items() if deck.label.lower() == heading), None)
            continue
        if stripped.startswith("```"):
            in_fence = not in_fence
            continue
        if not in_fence:
            continue
        if stripped:
            line = raw if raw.endswith("\n") else raw + "\n"
            if current_deck:
                line = f"{DECK_HINT_PREFIX}{current_deck}]] {line}"
            extracted.append(line)

    return extracted


def _join_remote_path(parent: str, child: str) -> str:
    if not parent or parent == "/":
        return "/" + child
    if parent.endswith("/"):
        return parent + child
    return parent + "/" + child


def _normalize_target_root(target_path: str, forced_deck: str | None) -> str:
    value = _normalize_path(target_path)
    if forced_deck:
        return value

    for deck in DECKS.values():
        suffix = "/" + deck.folder
        if value.endswith(suffix):
            trimmed = value[: -len(suffix)]
            return trimmed or "/"
    return value


def _detect_decks(text: str) -> List[str]:
    normalized = " ".join(text.strip().split())
    if not normalized:
        return []

    if CYRILLIC_RE.search(normalized):
        return ["ukrainian"]

    tokens = [token.lower() for token in LATIN_TOKEN_RE.findall(normalized)]
    if not tokens:
        return []

    joined = " ".join(tokens)
    german_score = 0
    english_score = 0

    if any(ch in joined for ch in "äöüß"):
        german_score += 4
    if joined.startswith("sich "):
        german_score += 4
    if any(token in GERMAN_WORDS for token in tokens):
        german_score += 2
    if any(token in ENGLISH_WORDS for token in tokens):
        english_score += 2
    if any(joined.endswith(suffix) for suffix in GERMAN_SUFFIXES):
        german_score += 1
    if any(joined.endswith(suffix) for suffix in ENGLISH_SUFFIXES):
        english_score += 1
    if any(fragment in joined for fragment in GERMAN_FRAGMENTS):
        german_score += 1
    if any(fragment in joined for fragment in ENGLISH_FRAGMENTS):
        english_score += 1

    if german_score > english_score + 1:
        return ["german"]
    if english_score > german_score + 1:
        return ["english"]

    # Latin-only terms without strong evidence are treated as ambiguous.
    return ["german", "english"]


def _resolve_decks(term: str, forced_deck: str | None, ambiguous_policy: str) -> List[str]:
    if forced_deck:
        return [forced_deck]

    decks = _detect_decks(term)
    if len(decks) <= 1:
        return decks
    if ambiguous_policy == "skip":
        return []
    if ambiguous_policy == "first":
        return [decks[0]]
    return decks


def _target_lang_for_deck(deck_key: str, target_lang_override: str | None) -> str:
    if target_lang_override:
        return target_lang_override
    env_key = f"CROSSPOINT_{deck_key.upper()}_TARGET_LANG"
    return os.environ.get(env_key, DECKS[deck_key].default_target_lang)


def _source_lang_for_deck(deck_key: str, source_lang_override: str | None) -> str:
    if source_lang_override:
        return source_lang_override
    return DECKS[deck_key].source_lang


def _remote_path_for_deck(target_path: str, deck_key: str, forced_deck: str | None) -> str:
    deck = DECKS[deck_key]
    if forced_deck:
        base = _normalize_path(target_path)
        if base.endswith("/" + deck.folder):
            return base
        return _normalize_path(_join_remote_path(base, deck.folder))

    root = _normalize_target_root(target_path, forced_deck=None)
    return _normalize_path(_join_remote_path(root, deck.folder))


def collect_pairs_by_deck(
    lines: Iterable[str],
    *,
    timeout_sec: int = 20,
    source_lang_override: str | None = None,
    target_lang_override: str | None = None,
    auto_translate: bool = True,
    forced_deck: str | None = None,
    ambiguous_policy: str = "multi",
) -> Tuple[Dict[str, List[Pair]], SyncSummary]:
    pairs_by_deck: Dict[str, List[Pair]] = {deck_key: [] for deck_key in DECKS}
    seen_by_deck: Dict[str, set[Pair]] = {deck_key: set() for deck_key in DECKS}
    translation_cache: dict[Tuple[str, str, str], str] = {}
    summary = SyncSummary()

    for raw in lines:
        parsed = _parse_input_line(raw)
        if parsed is None:
            continue

        term, explicit_translation, deck_hint = parsed
        deck_keys = [deck_hint] if deck_hint else _resolve_decks(term, forced_deck=forced_deck, ambiguous_policy=ambiguous_policy)
        if not deck_keys:
            summary.skipped += 1
            summary.unrouted_lines += 1
            print(f"[warn] Could not route '{term}' to a deck.", file=sys.stderr)
            continue

        if len(deck_keys) > 1:
            summary.ambiguous_lines += 1
            print(f"[warn] Ambiguous deck for '{term}'; routing to {', '.join(deck_keys)}.", file=sys.stderr)

        if explicit_translation is None and not auto_translate:
            summary.skipped += 1
            print(f"[warn] Skipping '{term}' because auto-translation is disabled.", file=sys.stderr)
            continue

        added_to_any_deck = False

        for deck_key in deck_keys:
            if explicit_translation is None:
                source_lang = _source_lang_for_deck(deck_key, source_lang_override)
                target_lang = _target_lang_for_deck(deck_key, target_lang_override)
                cache_key = (term, source_lang, target_lang)
                if cache_key in translation_cache:
                    translation = translation_cache[cache_key]
                else:
                    try:
                        translation = _translate_mymemory(term, source_lang, target_lang, timeout_sec)
                    except Exception as exc:
                        summary.translation_errors += 1
                        print(
                            f"[warn] Auto-translation failed for '{term}' ({source_lang}->{target_lang}): {exc}",
                            file=sys.stderr,
                        )
                        continue
                    translation_cache[cache_key] = translation
                    summary.translated_count += 1
                candidate = (term, translation)
            else:
                candidate = (term, explicit_translation)

            if candidate in seen_by_deck[deck_key]:
                continue

            seen_by_deck[deck_key].add(candidate)
            pairs_by_deck[deck_key].append(candidate)
            added_to_any_deck = True

        if not added_to_any_deck:
            summary.skipped += 1

    return pairs_by_deck, summary


def render_markdown_export(
    pairs_by_deck: Dict[str, Sequence[Pair]],
    summary: SyncSummary,
    *,
    title: str = "Flashcards Export",
    generated_at: str | None = None,
) -> str:
    generated_label = generated_at or dt.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"
    total_cards = sum(len(pairs) for pairs in pairs_by_deck.values())

    lines = [
        f"# {title}",
        "",
        f"Generated: {generated_label}",
        f"Total cards: {total_cards}",
        "",
        "This file contains explicit `term<TAB>translation` pairs inside fenced `tsv` blocks.",
        "You can feed it back into `quizlet_sync.py --pairs-file export.md --no-auto-translate`.",
        "",
    ]

    for deck_key in DECKS:
        pairs = list(pairs_by_deck.get(deck_key, []))
        if not pairs:
            continue

        lines.extend([f"## {DECKS[deck_key].label}", "", "```tsv"])
        for term, translation in pairs:
            lines.append(f"{term}\t{translation}")
        lines.extend(["```", ""])

    lines.extend(["## Summary", ""])
    lines.append(f"- Exported cards: {total_cards}")
    if summary.translated_count:
        lines.append(f"- Auto-translated lines: {summary.translated_count}")
    if summary.ambiguous_lines:
        lines.append(f"- Ambiguous lines routed: {summary.ambiguous_lines}")
    if summary.skipped:
        lines.append(f"- Skipped lines: {summary.skipped}")
    if summary.translation_errors:
        lines.append(f"- Translation failures: {summary.translation_errors}")
    if summary.unrouted_lines:
        lines.append(f"- Unrouted lines: {summary.unrouted_lines}")

    return "\n".join(lines).rstrip() + "\n"


def sync_lines(
    lines: Iterable[str],
    *,
    prefix: str,
    output_dir: str,
    host: str,
    target_path: str,
    timeout_sec: int = 20,
    source_lang_override: str | None = None,
    target_lang_override: str | None = None,
    auto_translate: bool = True,
    no_upload: bool = False,
    date_tag: str | None = None,
    forced_deck: str | None = None,
    ambiguous_policy: str = "multi",
    web_host: str = "",
    web_token: str = "",
    no_web_upload: bool = False,
) -> SyncSummary:
    pairs_by_deck, summary = collect_pairs_by_deck(
        lines,
        timeout_sec=timeout_sec,
        source_lang_override=source_lang_override,
        target_lang_override=target_lang_override,
        auto_translate=auto_translate,
        forced_deck=forced_deck,
        ambiguous_policy=ambiguous_policy,
    )

    if not any(pairs_by_deck.values()):
        return summary

    normalized_host = _normalize_host(host)
    date_tag = date_tag or dt.date.today().isoformat()

    for deck_key, pairs in pairs_by_deck.items():
        if not pairs:
            continue

        deck = DECKS[deck_key]
        deck_output_dir = os.path.join(output_dir, deck.folder)
        out_path = _build_output_path(deck_output_dir, prefix, date_tag)
        _write_quizlet_file(out_path, pairs)

        remote_path = _remote_path_for_deck(target_path, deck_key, forced_deck)

        web_uploaded = False
        web_path = ""
        web_error: str | None = None
        if web_host and not no_web_upload:
            try:
                web_path = _upload_to_web(_normalize_host(web_host), web_token, deck_key, out_path, timeout_sec)
                web_uploaded = True
            except Exception as exc:
                web_error = str(exc)
                print(f"[warn] Web upload failed for {deck_key}: {exc}", file=sys.stderr)

        uploaded = False
        if not no_upload:
            try:
                _ensure_remote_path(normalized_host, remote_path, timeout_sec=timeout_sec)
                status, payload = _upload_file(normalized_host, remote_path, out_path, timeout_sec=timeout_sec)
                if not 200 <= status < 300:
                    raise RuntimeError(f"HTTP {status} {payload.strip()}")
                uploaded = True
            except Exception as exc:
                if web_uploaded:
                    # The sync agent pushes web-only files to the device on its next
                    # pass, so a sleeping/offline device is not a hard failure.
                    print(
                        f"[warn] Device upload failed for {remote_path} ({exc}); "
                        "file is on the web and will reach the device via the sync agent.",
                        file=sys.stderr,
                    )
                else:
                    raise RuntimeError(f"Upload failed for {remote_path}: {exc}")

        if web_error and not uploaded and not web_uploaded:
            raise RuntimeError(f"Both device and web uploads failed for {deck_key}: {web_error}")

        summary.results.append(
            DeckResult(
                deck=deck_key,
                local_path=out_path,
                remote_path=remote_path,
                cards=len(pairs),
                uploaded=uploaded,
                web_uploaded=web_uploaded,
                web_path=web_path,
            )
        )

    return summary


def _print_summary(summary: SyncSummary, host: str) -> None:
    for result in summary.results:
        deck = DECKS[result.deck]
        print(f"[ok] {deck.label}: {result.cards} cards")
        print(f"[ok] Local file: {result.local_path}")
        if result.uploaded:
            print(f"[ok] Uploaded to: {host}{result.remote_path}/{os.path.basename(result.local_path)}")
        else:
            print(f"[ok] Target path: {result.remote_path}")
        if result.web_uploaded:
            print(f"[ok] Web upload: {result.web_path}")

    if summary.translated_count:
        print(f"[ok] Auto-translated lines: {summary.translated_count}")
    if summary.ambiguous_lines:
        print(f"[warn] Ambiguous lines routed using policy: {summary.ambiguous_lines}")
    if summary.skipped:
        print(f"[warn] Skipped lines: {summary.skipped}")
    if summary.translation_errors:
        print(f"[warn] Translation failures: {summary.translation_errors}")
    if summary.unrouted_lines:
        print(f"[warn] Unrouted lines: {summary.unrouted_lines}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Create Quizlet TXT files and upload them to CrossPoint decks.")
    parser.add_argument("--pairs-file", help="Input text file with one card per line.")
    parser.add_argument("--prefix", default="flashcards", help="Output filename prefix. Default: flashcards")
    parser.add_argument("--output-dir", default=".", help="Local directory for generated TXT files.")
    parser.add_argument(
        "--host",
        default=os.environ.get("CROSSPOINT_DEVICE", ""),
        help="CrossPoint device base URL, e.g. http://192.168.1.50 (default: CROSSPOINT_DEVICE env).",
    )
    parser.add_argument("--target-path", default="/flashcards", help="Base remote flashcards folder.")
    parser.add_argument("--date", dest="date_override", help="Override date tag (YYYY-MM-DD) for reproducible runs.")
    parser.add_argument("--timeout", type=int, default=20, help="HTTP timeout seconds.")
    parser.add_argument("--source-lang", help="Override source language for auto-translation.")
    parser.add_argument("--target-lang", help="Override target language for auto-translation.")
    parser.add_argument(
        "--deck",
        choices=sorted(DECKS.keys()),
        help="Force every input line into a single deck instead of routing by detected language.",
    )
    parser.add_argument(
        "--ambiguous-policy",
        choices=("multi", "skip", "first"),
        default="multi",
        help="What to do when deck language is ambiguous. Default: multi",
    )
    parser.add_argument(
        "--no-auto-translate",
        action="store_true",
        help="Disable auto-translation for lines without explicit pairs.",
    )
    parser.add_argument("--no-upload", action="store_true", help="Only generate local TXT files.")
    parser.add_argument(
        "--web-host",
        default=os.environ.get("CROSSPOINT_WEB", ""),
        help="Flashcards web app base URL (default: CROSSPOINT_WEB env). Empty disables web upload.",
    )
    parser.add_argument(
        "--web-token",
        default=os.environ.get("CROSSPOINT_WEB_TOKEN", ""),
        help="API token for the web app (default: CROSSPOINT_WEB_TOKEN env).",
    )
    parser.add_argument("--no-web-upload", action="store_true", help="Skip uploading decks to the web app.")
    parser.add_argument(
        "--sync-only",
        action="store_true",
        help="Skip card input entirely; just run one device<->web sync pass.",
    )
    parser.add_argument(
        "--no-sync",
        action="store_true",
        help="Do not run the device<->web sync pass after uploading.",
    )
    parser.add_argument(
        "--sync-agent",
        default=DEFAULT_SYNC_AGENT,
        help="Path to sync_agent.py (default: CROSSPOINT_SYNC_AGENT env or the crosspoint-flashcards-web checkout).",
    )
    args = parser.parse_args()

    if not args.host and not (args.no_upload and args.no_sync):
        if args.sync_only or not args.no_upload or not args.no_sync:
            print("ERROR: Set CROSSPOINT_DEVICE or pass --host (or use --no-upload --no-sync).", file=sys.stderr)
            return 2

    if args.sync_only:
        ok = run_device_web_sync(_normalize_host(args.host), args.web_host, args.web_token, args.sync_agent)
        return 0 if ok else 1

    if not args.pairs_file and sys.stdin.isatty():
        print("ERROR: Provide --pairs-file or pipe pair lines via stdin.", file=sys.stderr)
        return 2

    try:
        lines = _load_input_lines(args.pairs_file)
    except OSError as exc:
        print(f"ERROR: Failed to read input: {exc}", file=sys.stderr)
        return 2

    try:
        normalized_host = _normalize_host(args.host)
        summary = sync_lines(
            lines=lines,
            prefix=args.prefix,
            output_dir=args.output_dir,
            host=normalized_host,
            target_path=args.target_path,
            timeout_sec=args.timeout,
            source_lang_override=args.source_lang,
            target_lang_override=args.target_lang,
            auto_translate=not args.no_auto_translate,
            no_upload=args.no_upload,
            date_tag=args.date_override,
            forced_deck=args.deck,
            ambiguous_policy=args.ambiguous_policy,
            web_host=args.web_host,
            web_token=args.web_token,
            no_web_upload=args.no_web_upload,
        )
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if not summary.results:
        print("ERROR: No valid pairs found in input.", file=sys.stderr)
        return 2

    _print_summary(summary, normalized_host)

    if not args.no_sync:
        run_device_web_sync(normalized_host, args.web_host, args.web_token, args.sync_agent)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
