#!/usr/bin/env python3
"""Import Kindle Vocabulary Builder words into CrossPoint flashcards.

Kindle stores every word you look up while reading in a small SQLite database,
``system/vocabulary/vocab.db`` on the device. This script reads the words added
since its last run, routes each to a CrossPoint deck by language, and hands them
to ``quizlet_sync.py`` — which auto-translates the single terms (MyMemory),
writes deck TXT files, and uploads them to the web app (and the device if awake).
It then records the newest timestamp it processed so the next run only picks up
words looked up since.

Zero third-party dependencies (Python 3.10+ stdlib only).

Typical use (Kindle plugged in via USB):

    python3 kindle_vocab_sync.py                 # detect Kindle, sync new words
    python3 kindle_vocab_sync.py --emit-only     # just print what it would import
    python3 kindle_vocab_sync.py --all           # (re)import every word, ignore state

Credentials/host resolve the same way as sync_agent.py: env
CROSSPOINT_WEB / CROSSPOINT_WEB_TOKEN / CROSSPOINT_DEVICE, then
~/.crosspoint_sync/config.json (keys web / web_token / device).
"""

from __future__ import annotations

import argparse
import datetime as dt
import glob
import json
import os
import sqlite3
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# Kindle language code (first subtag, lowercased) -> CrossPoint deck id.
# Words in any other language are skipped (there is no deck for them).
DEFAULT_LANG_TO_DECK: Dict[str, str] = {
    "de": "german",
    "uk": "ukrainian",
    "en": "english",
}

CONFIG_PATH = os.path.expanduser(os.environ.get("CROSSPOINT_CONFIG", "~/.crosspoint_sync/config.json"))
STATE_PATH = os.path.expanduser(os.environ.get("CROSSPOINT_KINDLE_STATE", "~/.crosspoint_sync/kindle_state.json"))

# Where a mounted Kindle exposes the vocabulary DB. Globs cover the volume being
# named "Kindle", "Kindle1", etc. on macOS; Linux mounts are added for portability.
DB_GLOBS = [
    "/Volumes/Kindle*/system/vocabulary/vocab.db",
    "/Volumes/Kindle*/System/vocabulary/vocab.db",
    "/media/*/Kindle/system/vocabulary/vocab.db",
    "/run/media/*/Kindle/system/vocabulary/vocab.db",
]


def _load_config_file() -> Dict[str, str]:
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        if isinstance(data, dict):
            return {str(k): str(v) for k, v in data.items() if isinstance(v, (str, int))}
    except (OSError, json.JSONDecodeError):
        pass
    return {}


def _resolve(cli_value: Optional[str], env_key: str, config_key: str, config: Dict[str, str]) -> str:
    if cli_value:
        return cli_value
    return os.environ.get(env_key) or config.get(config_key, "")


def detect_db() -> Optional[str]:
    """Return the first existing Kindle vocab.db path, or None."""
    for pattern in DB_GLOBS:
        for match in sorted(glob.glob(pattern)):
            if os.path.isfile(match):
                return match
    fallback = os.path.expanduser("~/.crosspoint_sync/vocab.db")
    return fallback if os.path.isfile(fallback) else None


def load_state() -> Dict[str, object]:
    try:
        with open(STATE_PATH, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        if isinstance(data, dict):
            return data
    except (OSError, json.JSONDecodeError):
        pass
    return {}


def save_state(last_ts: int, db_path: str) -> None:
    os.makedirs(os.path.dirname(STATE_PATH), exist_ok=True)
    tmp = f"{STATE_PATH}.tmp-{os.getpid()}"
    payload = {
        "last_ts": int(last_ts),
        "last_run": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "db_path": db_path,
    }
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=1)
    os.replace(tmp, STATE_PATH)


class Word:
    __slots__ = ("term", "lang", "deck", "category", "timestamp", "usage")

    def __init__(self, term: str, lang: str, deck: str, category: int, timestamp: int, usage: str):
        self.term = term
        self.lang = lang
        self.deck = deck
        self.category = category
        self.timestamp = timestamp
        self.usage = usage


def read_words(
    db_path: str,
    since_ts: int,
    lang_to_deck: Dict[str, str],
    word_field: str,
) -> Tuple[List[Word], int, Dict[str, int]]:
    """Read WORDS newer than since_ts. Returns (routed words, max_ts_seen, skipped-by-lang counts)."""
    # Open read-only so a mounted Kindle volume is never modified/locked.
    uri = f"file:{Path(db_path).resolve()}?mode=ro"
    conn = sqlite3.connect(uri, uri=True)
    try:
        conn.row_factory = sqlite3.Row
        tables = {r[0] for r in conn.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        if "WORDS" not in tables:
            raise RuntimeError("not a Kindle vocab.db (no WORDS table)")
        has_lookups = "LOOKUPS" in tables

        usage_expr = (
            "(SELECT l.usage FROM LOOKUPS l WHERE l.word_key = w.id ORDER BY l.timestamp DESC LIMIT 1)"
            if has_lookups
            else "''"
        )
        rows = conn.execute(
            f"""
            SELECT w.word AS word, w.stem AS stem, w.lang AS lang,
                   COALESCE(w.category, 0) AS category, COALESCE(w.timestamp, 0) AS timestamp,
                   {usage_expr} AS usage
            FROM WORDS w
            WHERE COALESCE(w.timestamp, 0) > ?
            ORDER BY w.timestamp ASC
            """,
            (since_ts,),
        ).fetchall()
    finally:
        conn.close()

    words: List[Word] = []
    skipped: Dict[str, int] = {}
    max_ts = since_ts
    for row in rows:
        ts = int(row["timestamp"] or 0)
        max_ts = max(max_ts, ts)
        raw_lang = (row["lang"] or "").strip().lower()
        lang = raw_lang.split("-")[0]
        term = ((row[word_field] if word_field in row.keys() else None) or row["word"] or row["stem"] or "").strip()
        if not term:
            continue
        deck = lang_to_deck.get(lang)
        if not deck:
            skipped[lang or "?"] = skipped.get(lang or "?", 0) + 1
            continue
        words.append(Word(term, lang, deck, int(row["category"] or 0), ts, (row["usage"] or "").strip()))
    return words, max_ts, skipped


def build_input_lines(words: List[Word]) -> List[str]:
    """One `[[deck:<deck>]] <term>` line per unique (deck, term). Deck hint makes
    quizlet_sync route by Kindle's own language rather than guessing."""
    seen = set()
    lines: List[str] = []
    for w in words:
        key = (w.deck, w.term.lower())
        if key in seen:
            continue
        seen.add(key)
        lines.append(f"[[deck:{w.deck}]] {w.term}")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", help="Path to Kindle vocab.db (default: autodetect a mounted Kindle).")
    parser.add_argument("--all", action="store_true", help="Import every word, ignoring saved state.")
    parser.add_argument("--since-ts", type=int, help="Only import words with timestamp (ms) greater than this.")
    parser.add_argument(
        "--word-field",
        choices=("stem", "word"),
        default="stem",
        help="Which form to study: dictionary base form (stem, default) or the exact selected word.",
    )
    parser.add_argument("--emit-only", action="store_true", help="Print the deck-tagged lines and exit (no upload, no state write).")
    parser.add_argument("--dry-run", action="store_true", help="Do everything except upload and state write; prints the quizlet_sync command.")
    parser.add_argument("--output-dir", default=os.path.expanduser("~/.crosspoint_sync/kindle_decks"), help="Local dir for generated deck TXT files.")
    parser.add_argument("--prefix", default="kindle", help="Deck filename prefix. Default: kindle")
    # Host/credentials (fall through to env then config.json).
    parser.add_argument("--device", help="Device base URL (default: CROSSPOINT_DEVICE env / config).")
    parser.add_argument("--web", help="Web app base URL (default: CROSSPOINT_WEB env / config).")
    parser.add_argument("--token", help="Web app API token (default: CROSSPOINT_WEB_TOKEN env / config).")
    parser.add_argument("--no-device", action="store_true", help="Do not upload to / sync with the device; web app only.")
    parser.add_argument("--no-web-upload", action="store_true", help="Do not upload to the web app.")
    parser.add_argument("--no-sync", action="store_true", help="Do not run the device<->web sync pass after upload.")
    parser.add_argument("--timeout", type=int, default=20, help="HTTP timeout seconds passed to quizlet_sync.")
    parser.add_argument("--quizlet-sync", default=str(Path(__file__).with_name("quizlet_sync.py")), help="Path to quizlet_sync.py.")
    args = parser.parse_args()

    config = _load_config_file()
    db_path = args.db or os.environ.get("KINDLE_VOCAB_DB") or detect_db()
    if not db_path:
        print(
            "ERROR: No Kindle vocab.db found. Plug in your Kindle (it mounts at /Volumes/Kindle),\n"
            "       or pass --db /path/to/vocab.db, or copy it to ~/.crosspoint_sync/vocab.db.",
            file=sys.stderr,
        )
        return 2
    if not os.path.isfile(db_path):
        print(f"ERROR: vocab.db not found at {db_path}", file=sys.stderr)
        return 2

    state = load_state()
    if args.since_ts is not None:
        since_ts = args.since_ts
    elif args.all:
        since_ts = 0
    else:
        since_ts = int(state.get("last_ts", 0) or 0)

    try:
        words, max_ts, skipped = read_words(db_path, since_ts, DEFAULT_LANG_TO_DECK, args.word_field)
    except Exception as exc:  # noqa: BLE001 - surface any DB problem clearly
        print(f"ERROR: could not read {db_path}: {exc}", file=sys.stderr)
        return 1

    print(f"[ok] Kindle DB: {db_path}")
    print(f"[ok] Words since ts {since_ts}: {len(words)} routable" + (f", skipped by language: {skipped}" if skipped else ""))
    for deck in sorted(DEFAULT_LANG_TO_DECK.values()):
        n = sum(1 for w in words if w.deck == deck)
        if n:
            print(f"     {deck}: {n}")

    if not words:
        print("[ok] Nothing new to import.")
        return 0

    lines = build_input_lines(words)

    if args.emit_only:
        for line in lines:
            print(line)
        return 0

    web = _resolve(args.web, "CROSSPOINT_WEB", "web", config)
    token = _resolve(args.token, "CROSSPOINT_WEB_TOKEN", "web_token", config)
    device = _resolve(args.device, "CROSSPOINT_DEVICE", "device", config)

    cmd = [sys.executable, args.quizlet_sync, "--prefix", args.prefix, "--output-dir", args.output_dir, "--timeout", str(args.timeout)]
    use_device = bool(device) and not args.no_device
    use_web = bool(web) and not args.no_web_upload
    if use_device:
        cmd += ["--host", device]
    else:
        # quizlet_sync requires a host unless both device upload and sync are off.
        cmd += ["--no-upload", "--no-sync"]
    if args.no_sync and use_device:
        cmd += ["--no-sync"]
    if use_web:
        cmd += ["--web-host", web]
    else:
        cmd += ["--no-web-upload"]

    child_env = os.environ.copy()
    if token:
        child_env["CROSSPOINT_WEB_TOKEN"] = token

    stdin_text = "\n".join(lines) + "\n"

    if args.dry_run:
        print("[dry-run] would run:", " ".join(cmd))
        print("[dry-run] stdin:")
        print(stdin_text, end="")
        return 0

    if not use_device and not use_web:
        print("ERROR: nothing to upload to; configure a device or web host.", file=sys.stderr)
        return 2

    print(f"[ok] Handing {len(lines)} term(s) to quizlet_sync (auto-translate + upload)...", flush=True)
    result = subprocess.run(cmd, input=stdin_text, text=True, env=child_env)
    if result.returncode != 0:
        print(f"ERROR: quizlet_sync exited {result.returncode}; state not advanced (will retry these words next run).", file=sys.stderr)
        return result.returncode

    save_state(max_ts, db_path)
    print(f"[ok] Imported through ts {max_ts} ({_iso_ms(max_ts)}). State saved to {STATE_PATH}.")
    return 0


def _iso_ms(ms: int) -> str:
    if not ms:
        return "epoch"
    try:
        return dt.datetime.fromtimestamp(ms / 1000).astimezone().isoformat(timespec="seconds")
    except (OverflowError, OSError, ValueError):
        return str(ms)


if __name__ == "__main__":
    raise SystemExit(main())
