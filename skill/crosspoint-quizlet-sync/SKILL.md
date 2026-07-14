---
name: crosspoint-quizlet-sync
description: Build Quizlet-compatible flashcard TXT files for a CrossPoint device and the companion flashcards web app from German, Ukrainian, or English vocabulary, explicit term-translation pairs, short vocab lists pasted directly into chat, or words looked up in a Kindle's Vocabulary Builder (system/vocabulary/vocab.db). Use when the agent should turn study words into dated deck files, route them into the correct fixed deck folder by language, optionally auto-translate or normalize obvious typos, save local .txt files, and upload them to the CrossPoint device and/or the web app. Also use to import Kindle looked-up words (run scripts/kindle_vocab_sync.py), or when the user asks to sync flashcards/learning progress between the device and the web app (run with --sync-only).
---

# CrossPoint Quizlet Sync

Convert vocab input into one or more Quizlet-compatible TSV deck files and upload them to a CrossPoint device (Xteink X4 running the flashcards firmware from this repo) and/or the companion web app (`web/` in this repo).

## Configuration

Set these in the environment (e.g. shell profile):

- `CROSSPOINT_DEVICE` — device base URL on your LAN, e.g. `http://192.168.1.50`
- `CROSSPOINT_WEB` — web app base URL (Railway/any host running `web/server.js`)
- `CROSSPOINT_WEB_TOKEN` — the web app's `API_TOKEN`
- `CROSSPOINT_SYNC_AGENT` — optional path to `sync_agent.py` (defaults to `web/agent/sync_agent.py` in this repo)

The e-reader sleeps aggressively. If HTTP to the device times out, it is asleep or off WiFi — uploads to the web app still work and the next sync pass delivers them to the device. Quick check: `curl -m 3 $CROSSPOINT_DEVICE/api/status` (HTTP 200 = awake).

## Workflow

1. Normalize the input.
- Accept `term<TAB>translation`, `term,translation`, `term::translation`, or single German/Ukrainian/English terms.
- Also accept a Markdown export file from the Telegram bot when the explicit pairs are inside fenced `tsv` code blocks.
- If the user pastes a comma-separated list in chat, split it into one item per line before running the script.
- Preserve Unicode. Skip blank lines and lines starting with `#`.
- Correct only obvious spelling mistakes when confidence is high. Mention those corrections in the final response.

2. Prefer explicit pairs when possible.
- If you already know the translations, convert the list to explicit `term<TAB>translation` pairs and run with `--no-auto-translate` for deterministic output.
- Use single-term input with auto-translation only when the user clearly wants speed and minor translation variance is acceptable.
- Remove exact duplicate pairs before writing the deck.

3. Route cards to decks.
- Fixed decks: `German` -> `/flashcards/german`, `Ukrainian` -> `/flashcards/ukrainian`, `English` -> `/flashcards/english` (same ids on the web app).
- The script classifies the prompt term by language and uploads each card to the matching deck folder.
- Ambiguous Latin-script terms default to `--ambiguous-policy multi`, so they may go to both `German` and `English`.
- Use `--deck german|ukrainian|english` to force everything into a single deck.

4. Run the script.

```bash
python3 scripts/quizlet_sync.py \
  --pairs-file /absolute/path/to/pairs.txt \
  --prefix vocab \
  --output-dir /tmp/crosspoint_flashcards
```

- Omit `--pairs-file` to read from stdin.
- Web upload happens automatically when `CROSSPOINT_WEB` + `CROSSPOINT_WEB_TOKEN` are set. Disable with `--no-web-upload`.
- If the device is asleep but the web upload succeeded, the script warns instead of failing — the next sync pass pushes the file to the device.
- Local files are written to `OUTPUT_DIR/<deck>/<prefix>-YYYY-MM-DD.txt`, then `-2`, `-3`, and so on if needed.
- Default auto-translation targets: German -> English, Ukrainian -> English, English -> German. Override per deck with `CROSSPOINT_<DECK>_TARGET_LANG`.

5. Confirm the result.
- Report every local file path that was created.
- Report every uploaded device path and web path.
- If a destination failed, say which one and note that the next sync pass reconciles device<->web.

## Key flags

- `--pairs-file PATH`, `--prefix PREFIX`, `--output-dir DIR`
- `--host URL` (device; default `CROSSPOINT_DEVICE`)
- `--web-host URL` / `--web-token TOKEN` (default `CROSSPOINT_WEB` / `CROSSPOINT_WEB_TOKEN`)
- `--deck german|ukrainian|english`, `--ambiguous-policy multi|skip|first`
- `--source-lang CODE`, `--target-lang CODE`, `--no-auto-translate`
- `--no-upload` (skip device), `--no-web-upload` (skip web)
- `--sync-only` (no card input; just run one device<->web sync pass)
- `--no-sync` (skip the automatic post-upload sync pass)
- `--sync-agent PATH` (override sync agent location)

## Device <-> web sync

`quizlet_sync.py` automatically runs one sync pass (via `web/agent/sync_agent.py`) after uploading decks; run `--sync-only` for a pure sync. Per deck: TXT files travel both directions (deletions propagate, device wins conflicts) and SM-2++ progress is merged (the more-reviewed record per card wins), written back only when changed. Idempotent. See `web/README.md` for details.

## Import from Kindle Vocabulary Builder

Use `scripts/kindle_vocab_sync.py` to pull the words you look up while reading on a Kindle into your decks. Kindle stores every looked-up word in a small SQLite DB (`system/vocabulary/vocab.db`); this script reads the words added since its last run, routes each to a deck by Kindle's own language tag, and hands them to `quizlet_sync.py` (which auto-translates the single terms and uploads). Some lag is fine — run it whenever the Kindle is plugged in.

```bash
python3 scripts/kindle_vocab_sync.py               # detect a mounted Kindle, import new words
python3 scripts/kindle_vocab_sync.py --emit-only   # preview the deck-tagged lines, no upload
python3 scripts/kindle_vocab_sync.py --all         # re-import every word (ignore saved state)
python3 scripts/kindle_vocab_sync.py --db /path/to/vocab.db   # explicit DB path
```

- **DB discovery:** autodetects a mounted Kindle (`/Volumes/Kindle*/system/vocabulary/vocab.db`), else `KINDLE_VOCAB_DB` env, else `--db`, else `~/.crosspoint_sync/vocab.db` (handy: copy the file off the Kindle once). Opened read-only.
- **Incremental:** tracks the newest `WORDS.timestamp` it processed in `~/.crosspoint_sync/kindle_state.json`, so each run only imports words looked up since. State advances only when the upload succeeds (a sleeping device still counts as success — the web upload is what matters).
- **Routing:** Kindle `lang` → deck (`de`→German, `uk`→Ukrainian, `en`→English via `[[deck:…]]` hints). Words in other languages are skipped and counted.
- **Word form:** `--word-field stem` (default, dictionary base form, e.g. *ging*→*gehen*) or `--word-field word` (exact selected form).
- **Credentials/host** resolve like `sync_agent.py`: env `CROSSPOINT_WEB` / `CROSSPOINT_WEB_TOKEN` / `CROSSPOINT_DEVICE`, then `~/.crosspoint_sync/config.json` (`web` / `web_token` / `device`). Pass-through flags: `--no-device`, `--no-web-upload`, `--no-sync`, `--dry-run`.
- **Automatic (optional):** `scripts/com.crosspoint.kindle-vocab-sync.plist` is an event-driven launchd agent that runs one import whenever a volume mounts (i.e. when you plug in the Kindle) — no polling, no background drain. Edit the script path, then bootstrap it (see comments in the plist).

## Notes

- Emit Quizlet-compatible TSV using a tab separator to avoid comma ambiguity.
- Use `/flashcards` as the canonical remote root. The script creates missing deck folders via `/mkdir`.
- If parsing rejects some lines, continue with valid rows and print warnings.
- If no valid pairs remain, exit with non-zero status.
