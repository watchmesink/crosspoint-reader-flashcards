# CrossPoint Reader — Flashcards Edition

A [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) fork for the **Xteink X4** with offline flashcards, SM-2++ spaced repetition, device↔web sync, and vocabulary-import tools.

For the original reader features, build instructions, and flashing guide, see the [upstream project](https://github.com/crosspoint-reader/crosspoint-reader).

## Highlights

- Three on-device decks: German, Ukrainian, and English.
- Quizlet-compatible text files at `/flashcards/<deck>/*.txt`, using `term<TAB>translation`.
- Twenty-card study batches with Hard, Good, and Easy ratings; progress survives file moves and renames.
- Automatic wake-time sync for deck files, deletions, and learning progress.
- An offline-first companion PWA in [`web/`](./web) for studying and managing decks from a phone.
- A Codex/agent skill in [`skill/crosspoint-quizlet-sync/`](./skill/crosspoint-quizlet-sync) for creating, translating, routing, and syncing cards.
- Optional Telegram collection and Kindle Vocabulary Builder import, including a wireless workflow for jailbroken Kindles.

## Quick start

1. Build and flash the firmware as described upstream, or use a prebuilt `*-firmware.bin` from this repository.
2. Add a deck file such as `/flashcards/german/verbs.txt`:

   ```text
   anerkennen	to acknowledge
   der Zweifel	the doubt
   ```

3. Open **Flashcards** on the device and choose a deck.

For web sync, deploy [`web/`](./web) and place `/.crosspoint/flashcards_sync.json` on the SD card:

```json
{"url": "https://your-web-app.example", "token": "<API_TOKEN>", "enabled": true}
```

## Agent skill

The repository exposes `crosspoint-quizlet-sync` through [`.agents/skills/`](./.agents/skills/crosspoint-quizlet-sync), backed by the complete package in [`skill/`](./skill/crosspoint-quizlet-sync). To install it for every Codex task, symlink the package into your personal skills directory:

```bash
mkdir -p ~/.codex/skills
ln -s "$PWD/skill/crosspoint-quizlet-sync" ~/.codex/skills/crosspoint-quizlet-sync
```

The skill can create dated decks from pasted vocabulary, normalize German nouns, auto-translate, import Kindle lookups, upload to the web app or device, and merge study progress.

## Repository map

- [`src/`](./src): Xteink X4 firmware changes
- [`web/`](./web): offline-first PWA, server, and LAN sync agent
- [`skill/crosspoint-quizlet-sync/`](./skill/crosspoint-quizlet-sync): agent skill and import scripts
- [`docs/`](./docs): architecture notes

## License

MIT, matching upstream. See [LICENSE](./LICENSE).
