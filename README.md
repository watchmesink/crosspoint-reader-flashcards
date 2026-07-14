# CrossPoint Reader — Flashcards Edition

A fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) — the open-source firmware for the **Xteink X4** e-paper reader — that turns the device into a spaced-repetition vocabulary trainer with a companion web app.

This README covers **only what this fork adds**. For everything else (EPUB reading, file transfer, OPDS, KOReader sync, themes, build instructions, flashing), see the [upstream project](https://github.com/crosspoint-reader/crosspoint-reader).

## What's added

### 📇 Flashcards on the device

A new **Flashcards** mode on the home screen with three fixed decks — German, Ukrainian, English.

- Decks are plain text files on the SD card: `/flashcards/{german,ukrainian,english}/*.txt`, one card per line as `term<TAB>translation` (falls back to splitting on the last comma; `#` starts a comment). Up to 900 cards per deck across any number of files.
- Scheduling is **SM-2++**: learning steps → review phase with an ease factor (1.30–3.00), interval growth, lapses back to relearning, and deterministic interval fuzz. "Time" is the deck's review counter, not the wall clock, so the scheduler works on a device that sleeps most of the day.
- Cards are studied in **batches of 20**; flip with Up/Down and rate **Hard / Good / Easy** (only *Easy* clears a card from the batch). The start screen shows memorized count and a daily study streak.
- A card's identity is a hash of its content, so progress survives renaming, moving, or merging deck files. Progress is stored per deck in `/.crosspoint/flashcards_<deck>.bin`.

### 🔄 Device-initiated sync (on wake)

There is no always-on WiFi. Shortly after each boot/wake the firmware briefly brings WiFi up by itself (last saved network), runs one background sync pass against the companion web app, and powers the radio back off — no computer involved and no standby drain. If WiFi is already up for another reason (e.g. file transfer), the pass simply piggybacks on that connection:

- **Learning progress** is merged both ways (the more-reviewed record per card wins) and written back only when something changed.
- **Deck files** reconcile in both directions: files added on the web download to the device, device-only files upload, files deleted on the web are deleted on the device too, and content conflicts resolve in the device's favor.

Configure it by placing `/.crosspoint/flashcards_sync.json` on the SD card:

```json
{"url": "https://your-web-app.example", "token": "<API_TOKEN>", "enabled": true}
```

Without this file the feature is completely inert and the radio is never touched. One pass per connection, auto-sleep is held off while syncing, errors retry once after 5 minutes; look for `[FSY]` lines in the serial log.

### 📱 Companion web app — [`web/`](./web)

A zero-dependency Node.js server + mobile-first, **offline-first** PWA that mirrors the on-device experience: same decks, same batch mechanics, and an **exact port of the SM-2++ engine** (the test suite round-trips a real device progress file byte-identically). Once loaded it studies fully offline (service worker shell + IndexedDB, the same engine running in the browser) and reconciles with the server when the network returns. Study on your phone, manage deck files, set the batch size, and everything converges with the device on its next sync.

- Storage is plain files on a volume; deployable on Railway in a few commands (see [`web/README.md`](./web/README.md)).
- Browser access via a short PIN (exchanged server-side for the API token); scripts authenticate with a Bearer token.
- Also ships a LAN-side sync agent (`web/agent/sync_agent.py`) as an alternative/fallback to the firmware-initiated sync — all sync paths use the same idempotent merge and can coexist.

### 🤖 Agent skill — [`skill/crosspoint-quizlet-sync/`](./skill/crosspoint-quizlet-sync)

A skill for coding agents (Claude Code / Codex style) that turns vocabulary pasted into chat into dated Quizlet-style deck files, routes each card to the right deck by language detection (optionally auto-translating single terms), uploads to both the device and the web app, and triggers a sync. Includes an optional Telegram bot for collecting words on the go.

### 🛠 Versioned builds

Each build stamps an auto-incrementing `CROSSPOINT_VERSION` and exports the firmware as `<version>-firmware.bin` in the project root, ready to flash.

## User journey, step by step

1. **Collect words.** During the day you run into words you want to learn — paste them into a chat with your coding agent (the [skill](./skill/crosspoint-quizlet-sync) turns them into deck files, translating single terms if needed), type them into the web app's *Files* screen from your phone, or just drop a `.txt` onto the SD card.
2. **They reach both places.** The skill uploads the new deck file to the web app and the device (if it's awake); whatever is missing arrives with the next sync pass.
3. **Study on the e-reader.** Open **Flashcards** → pick a deck → *Learn*. Flip with Up/Down, rate with Hard / Good / Easy. Twenty-card batches; the start screen tracks memorized cards and your streak.
4. **Study on your phone.** Open the web app, enter your PIN, and continue exactly where the deck stands — same batch logic, same scheduler.
5. **Wake the device near WiFi.** It connects on its own, merges progress from both sides (per card, the more-reviewed record wins), pulls any new deck files, pushes its own, then switches the radio off. Open Flashcards and the phone reviews are reflected.
6. **Prune decks from the couch.** Delete a deck file in the web UI — on its next sync the device deletes its copy too (tombstones prevent it from re-uploading).

## Quick start

1. Build and flash like upstream CrossPoint (`pio run`), or grab a prebuilt `*-firmware.bin` from this repo.
2. Put deck files on the SD card, e.g. `/flashcards/german/verbs.txt`:

   ```
   anerkennen	to acknowledge
   der Zweifel	the doubt
   # comments and blank lines are ignored
   ```

3. Open **Flashcards** on the home screen and study.
4. Optional: deploy [`web/`](./web), drop `flashcards_sync.json` onto the SD card, and the device syncs itself on every wake.

## License

Same as upstream CrossPoint Reader (MIT) — see [LICENSE](./LICENSE).
