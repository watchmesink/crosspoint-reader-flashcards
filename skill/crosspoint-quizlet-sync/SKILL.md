---
name: crosspoint-quizlet-sync
description: Build Quizlet-compatible flashcard TXT files for a CrossPoint device and the flashcards web app from German, Ukrainian, or English vocabulary, explicit term-translation pairs, short vocab lists pasted directly into chat, or words looked up in a Kindle's Vocabulary Builder (system/vocabulary/vocab.db). Use when Codex should turn study words into dated deck files, route them into the correct fixed deck folder by language, optionally auto-translate or normalize obvious typos, save local .txt files, and upload them to the CrossPoint device and/or the web app. Also use to import Kindle looked-up words (run scripts/kindle_vocab_sync.py), or when the user asks to sync flashcards/learning progress between the device and the web app (run with --sync-only). Covers checking device connectivity too.
---

# CrossPoint Quizlet Sync

Convert vocab input into one or more Quizlet-compatible TSV deck files and upload them to CrossPoint (device) and the flashcards web app.

## Configuration

- Set `CROSSPOINT_DEVICE` to the Xteink X4 base URL on the LAN.
- Set `CROSSPOINT_WEB` and `CROSSPOINT_WEB_TOKEN` for the companion web app.
- Optionally set `CROSSPOINT_SYNC_AGENT` when `web/agent/sync_agent.py` is not available in this repository checkout.
- Alternatively store `device`, `web`, `web_token`, and `sync_agent` in `~/.crosspoint_sync/config.json`; this works in non-interactive shells that do not load shell profiles.
- Auto-translation sends terms to the public MyMemory translation API. Use explicit pairs with `--no-auto-translate` for sensitive vocabulary.

The device sleeps aggressively. The script probes it for about three seconds and, when it is offline, continues with a web-only upload. The firmware pulls new files and merges progress on its next wake sync.

## Workflow

1. Normalize the input.
- Accept `term<TAB>translation`, `term,translation`, `term::translation`, or single German/Ukrainian/English terms.
- Also accept a Markdown export file from the Telegram bot when the explicit pairs are inside fenced `tsv` code blocks.
- If the user pastes a comma-separated list in chat, split it into one item per line before running the script.
- Preserve Unicode. Skip blank lines and lines starting with `#`.
- Correct only obvious spelling mistakes when confidence is high. Mention those corrections in the final response.
- For German cards, add a missing article before a reliably identified bare noun (for example, `Nervenkitzel` -> `der Nervenkitzel`, `Genauigkeit` -> `die Genauigkeit`, `Fingerspitzengefühl` -> `das Fingerspitzengefühl`). Keep an article that is already present.
- Respect phrase grammar instead of blindly prepending a nominative article. Known collocations may use the required case (for example, `Aufwand betreiben` -> `einen Aufwand betreiben`); leave other multi-word phrases unchanged unless their case is certain.

2. Prefer explicit pairs when possible.
- If you already know the translations, convert the list to explicit `term<TAB>translation` pairs and run with `--no-auto-translate` for deterministic output.
- If the user provides the bot-generated `.md` export, pass it directly as `--pairs-file /absolute/path/export.md --no-auto-translate`.
- Use single-term input with auto-translation only when the user clearly wants speed and minor translation variance is acceptable.
- Remove exact duplicate pairs before writing the deck.

3. Route cards to decks.
- Default fixed decks on device:
  - `German` -> `/flashcards/german`
  - `Ukrainian` -> `/flashcards/ukrainian`
  - `English` -> `/flashcards/english`
- The web app uses the same deck ids (`german`, `ukrainian`, `english`).
- The script classifies the prompt term by language and uploads each card to the matching deck folder.
- Ambiguous Latin-script terms default to `--ambiguous-policy multi`, so they may go to both `German` and `English`.
- Use `--deck german|ukrainian|english` to force everything into a single deck when needed.

4. Run the script.

```bash
python3 scripts/quizlet_sync.py \
  --pairs-file /absolute/path/to/pairs.txt \
  --prefix telegram \
  --output-dir /tmp/crosspoint_flashcards
```

- Omit `--pairs-file` to read from stdin.
- Device and web endpoints resolve from environment variables or `~/.crosspoint_sync/config.json`.
- Default target root: `/flashcards`
- Web upload happens automatically (credentials from env or `~/.crosspoint_sync/config.json`; override with `--web-host` / `--web-token`, disable with `--no-web-upload`).
- The script probes the device first (~3s). If it is asleep, device uploads are skipped with a warning and the run still finishes in seconds — the device pulls the new files and merges progress on its next wake sync.
- Local files are written to `OUTPUT_DIR/<deck>/<prefix>-YYYY-MM-DD.txt`, then `-2`, `-3`, and so on if needed.
- Default auto-translation targets:
  - German -> English
  - Ukrainian -> English
  - English -> Russian
- Override a deck target with `CROSSPOINT_<DECK>_TARGET_LANG`, for example `CROSSPOINT_ENGLISH_TARGET_LANG=uk`.
- If `--pairs-file` points to a `.md` export from the Telegram bot, the script automatically extracts pair lines from fenced `tsv` blocks.
- `quizlet_sync.py` applies the German article-normalization rule before translation and writes the normalized term to the TSV file.

5. Confirm the result.
- Report every local file path that was created.
- Report every uploaded device path and web path.
- If a destination failed, say which one and note that the next sync pass reconciles device<->web.

## Script

Use `scripts/quizlet_sync.py`.

Key flags:
- `--pairs-file PATH`
- `--prefix PREFIX`
- `--output-dir DIR`
- `--host URL` (device)
- `--web-host URL` / `--web-token TOKEN` (web app; default from `CROSSPOINT_WEB` / `CROSSPOINT_WEB_TOKEN`)
- `--target-path PATH`
- `--deck german|ukrainian|english`
- `--ambiguous-policy multi|skip|first`
- `--source-lang CODE`
- `--target-lang CODE`
- `--no-auto-translate`
- `--no-upload` (skip device)
- `--no-web-upload` (skip web)
- `--sync-only` (no card input; just run one device<->web sync pass)
- `--no-sync` (skip the automatic post-upload sync pass)
- `--sync-agent PATH` (override the repo-relative `web/agent/sync_agent.py` path)

## Examples

Explicit pairs via stdin, routed by detected prompt language (uploads to device and web):

```bash
python3 scripts/quizlet_sync.py --prefix vocab --no-auto-translate <<'EOF'
zweifeln	to doubt
дякую	thank you
look up	aufschlagen
EOF
```

Single German terms with auto-translation:

```bash
cat <<'EOF' | python3 scripts/quizlet_sync.py --prefix german-b2
Verhältnis
Streit
sich ärgern über
EOF
```

Force everything into the Ukrainian deck:

```bash
python3 scripts/quizlet_sync.py --deck ukrainian --prefix ua-review --pairs-file /absolute/path/to/pairs.txt
```

Web only (device asleep, deliberate):

```bash
python3 scripts/quizlet_sync.py --prefix vocab --no-auto-translate --no-upload <<'EOF'
die Brücke	the bridge
EOF
```

## Device <-> web sync

Two independent, merge-safe paths keep device and web converged:

1. **Device-initiated (automatic):** firmware 0.1.14+ syncs whenever the device wakes and gets WiFi — it uploads device-only deck files, downloads web-only ones (i.e. words added via this skill while it slept), honors web deletion tombstones, and merges SM-2++ progress. Nothing to run on this side.
2. **Skill-triggered (on demand):** `quizlet_sync.py` automatically runs one sync pass after uploading decks; run it with `--sync-only` when the user just wants to sync (e.g. "sync my flashcards", "pull my progress from the device"). Only works while the device is awake on the LAN.

```bash
python3 scripts/quizlet_sync.py --sync-only        # uses CROSSPOINT_WEB / CROSSPOINT_WEB_TOKEN env
```

What one pass does per deck:

- TXT files travel in both directions; deletions propagate; on conflict the device wins.
- Learning progress (`/.crosspoint/flashcards_<deck>.bin`, SM-2++) is merged — the more-reviewed record per card wins — and written back to the device only when it changed. Idempotent: nothing is written when nothing changed.
- If the device is asleep/offline the pass reports it and exits cleanly; re-run when the device is awake.
- The device reads progress when the Flashcards activity is opened, so a sync that happens mid-study lands after the user re-opens Flashcards.

The sync agent and web app live under `web/` in this repository. Override the agent path with `CROSSPOINT_SYNC_AGENT` or `--sync-agent` when the skill is installed separately.

## Telegram Bot

Use `scripts/telegram_flashcards_bot.py` when the user wants to queue words from Telegram and export explicit pairs later.

Typical run:

```bash
TELEGRAM_BOT_TOKEN=... \
python3 scripts/telegram_flashcards_bot.py
```

Bot behavior:
- Plain text messages are queued per chat.
- `/pending` shows how many queued items remain.
- `/export` builds explicit term/translation pairs, groups them by deck, and sends one Markdown file back in Telegram.
- `/clear` empties the pending queue for that chat.

## Import from Kindle Vocabulary Builder

Use `scripts/kindle_vocab_sync.py` to pull words looked up while reading on a Kindle into the decks. Kindle stores every looked-up word in `system/vocabulary/vocab.db`; the script reads words added since its last run, routes each to a deck by Kindle's own language tag, and hands them to `quizlet_sync.py` (auto-translate single terms + upload). Some lag is fine — run it whenever the Kindle is plugged in.

```bash
python3 scripts/kindle_vocab_sync.py               # detect a mounted Kindle, import new words
python3 scripts/kindle_vocab_sync.py --emit-only   # preview the deck-tagged lines, no upload
python3 scripts/kindle_vocab_sync.py --all         # re-import every word (ignore saved state)
python3 scripts/kindle_vocab_sync.py --db /path/to/vocab.db
```

- Autodetects `/Volumes/Kindle*/system/vocabulary/vocab.db` (or `KINDLE_VOCAB_DB` / `--db` / `~/.crosspoint_sync/vocab.db`), opened read-only.
- Incremental via `~/.crosspoint_sync/kindle_state.json` (newest `WORDS.timestamp` processed); state advances only on a successful upload.
- Routes `de`→German, `uk`→Ukrainian, `en`→English (via `[[deck:…]]` hints); other languages skipped and counted.
- `--word-field stem` (default base form) or `--word-field word` (exact selected form). Credentials resolve from env then `~/.crosspoint_sync/config.json`, same as `sync_agent.py`.
- Optional `scripts/com.crosspoint.kindle-vocab-sync.plist`: event-driven launchd agent that imports on volume mount (plug in the Kindle) — no polling.

### Fully wireless & automatic (no USB, no manual run)

For a hands-free setup where the Kindle pushes its words on its own, see `kindle-device/`. The web app exposes `POST /api/kindle/vocab`: a **jailbroken** Kindle uploads its raw `vocab.db` over WiFi (via `kindle-device/upload_vocab.sh` run from `cron` on the device), and the server (`web/server.js`) parses new `WORDS` with the `sqlite3` CLI, translates them, and files them into the decks. Incremental server-side (watermark in `DATA_DIR/kindle/state.json`); `GET /api/kindle/status` shows progress. Requires a jailbroken Kindle (Amazon keeps `vocab.db` device-local, no cloud/API). Install steps: `kindle-device/README.md`.

## Notes

- Emit Quizlet-compatible TSV using a tab separator to avoid comma ambiguity.
- The Telegram bot export format is Markdown, but the actual pair data stays as tab-separated rows inside fenced `tsv` blocks.
- Use `/flashcards` as the canonical remote root. The script will create missing deck folders via `/mkdir`.
- If parsing rejects some lines, continue with valid rows and print warnings.
- If no valid pairs remain, exit with non-zero status.
