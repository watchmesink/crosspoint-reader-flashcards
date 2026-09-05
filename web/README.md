# CrossPoint Flashcards Web

Mobile-first, **offline-first** web app that mirrors the flashcards mode of the
CrossPoint firmware (Xteink X4 e-reader) and keeps decks + learning progress in
two-way sync with the device. It is an installable PWA: once loaded online it
studies fully offline (service worker shell + IndexedDB) and reconciles with the
server whenever the network returns.

The scheduling engine (`engine.js`) is a port of
`src/activities/flashcards/FlashcardsActivity.cpp` and is **isomorphic** — the
same source runs on the Node server and in the browser (a `Buffer`/`Uint8Array`
shim keeps the device bin byte-identical either way).

Two deliberate web-side divergences from the firmware (progress state stays
byte-compatible):

- **New uploads surface immediately**: each new batch reserves up to half its
  slots for never-reviewed cards from the newest files. The plain firmware ring
  would otherwise show a fresh upload only after a full rotation of the deck
  (~total/batchSize completed batches — weeks on a large deck).
- **Timezone-aware streak**: the server counts study days in `STREAK_TZ` (env
  var, IANA name like `Europe/Berlin`, default UTC), so studying just after
  local midnight no longer creates phantom gap days, and a lapsed streak is
  reported as 0 instead of the stale last value.

Firmware behavior otherwise preserved:

- Decks: `german`, `ukrainian`, `english`; cards are `prompt<TAB>translation`
  lines in `.txt` files (`#` comments, fallback split on last comma).
- Card identity: FNV-1a hash of `prompt\tanswer` — progress survives file
  renames/moves and is what makes device<->web merge possible.
- SM-2++: learning steps {1, 8, 48} measured in *review steps* (not days),
  Hard/Good/Easy ratings, ease 1.30–3.00, interval cap 4096, deterministic
  interval fuzz, configurable web batches of 1-20 cards, where only **Easy**
  marks a card processed.
- Progress binary: byte-compatible with `/.crosspoint/flashcards_<deck>.bin`
  (version 6) — the test suite round-trips a real device file byte-identically.

## Components

| Path | What it is |
|---|---|
| `server.js` | Zero-dependency Node server: REST API + static shell + progress merge |
| `engine.js` | Firmware engine port + bin codec + merge — **isomorphic** (Node `Buffer` / browser `Uint8Array` shim) so the server and browser run the same source |
| `public/index.html` | App shell (markup + styles); loads `/engine.js` + `/app.js` |
| `public/app.js` | Offline-first client: study/stats/files/settings over IndexedDB via the engine, plus reconcile |
| `public/sw.js` | Service worker: precaches the shell for offline; never caches `/api/*` |
| `public/manifest.webmanifest` | PWA manifest |
| `agent/sync_agent.py` | LAN-side sync agent (device ⇄ web), zero-dep Python |
| `test/test.js` | Engine tests incl. byte-identical round-trip of a real device bin + browser (`PortableBuffer`) parity |

## Server

```bash
API_TOKEN=secret DATA_DIR=/data PORT=8080 node server.js
```

- `API_TOKEN` — required in production; all `/api/*` (except `/api/health` and
  `/api/auth`) accept it as `Authorization: Bearer …` or `?token=…`.
- `PIN` — optional short unlock code for the browser UI. The SPA asks for it
  and exchanges it for the token via `POST /api/auth` (5 wrong attempts lock
  the IP for 15 minutes). Scripts keep using the Bearer token directly.
- `DATA_DIR` — persistent storage (Railway volume). Layout:
  `decks/<deck>/files/*.txt` + `decks/<deck>/progress.json`, plus a global
  `settings.json` (currently just `batchSize`).

### API

- `GET /api/decks` — summaries (total, memorized, due, streak, last device sync)
- `GET /api/decks/:deck` — deck view incl. current card
- `POST /api/decks/:deck/rate` `{key, rating: hard|good|easy}` — rate + advance
- `GET/PUT/DELETE /api/decks/:deck/files[/:name]` — deck TXT files
- `GET/POST /api/settings` — web app settings, including `batchSize` (1-20)
- `POST /api/kindle/vocab` — upload a Kindle `vocab.db` (raw body); server reads
  new looked-up words (via the `sqlite3` CLI), translates them, and files them
  into the decks. `?wait=1` blocks and returns the ingest summary. See
  [`../skill/crosspoint-quizlet-sync/kindle-device/`](../skill/crosspoint-quizlet-sync/kindle-device/).
- `GET /api/kindle/status` — last Kindle-import watermark
- `POST /api/sync/:deck/progress` `{bin: base64|null}` — merge the device's
  progress bin with web state; returns the merged bin to write back
- `GET /api/decks/:deck/progress.bin` — current web state as a device bin

## Sync agent (runs on a Mac in the same LAN)

```bash
python3 agent/sync_agent.py \
  --device http://<device-ip> \
  --web https://<railway-app> --token <API_TOKEN>          # one pass
python3 agent/sync_agent.py --watch                        # poll forever
```

Env defaults: `CROSSPOINT_DEVICE`, `CROSSPOINT_WEB`, `CROSSPOINT_WEB_TOKEN`.

Per pass and per deck the agent:

1. Two-way syncs `/flashcards/<deck>/*.txt` (new files travel in both
   directions; deletions propagate using the last-sync state in
   `~/.crosspoint_sync/state.json`; on content conflict the device wins).
2. Downloads `/.crosspoint/flashcards_<deck>.bin`, POSTs it to the web merge
   endpoint, and writes the merged bin back **only if it differs** — no flash
   wear when nothing changed. Merge rule: side with the higher review counter
   is primary; per card the more-reviewed record wins.

Install as a launchd service so a sync happens whenever the device joins the
network: see `agent/com.crosspoint.flashcards-sync.plist` (edit paths/URL,
copy to `~/Library/LaunchAgents`, `launchctl bootstrap gui/$UID <plist>`).

## Deploy (Railway)

```bash
railway init
railway volume add --mount-path /data
railway variables --set API_TOKEN=<secret> --set PIN=<pin> --set DATA_DIR=/data
railway up
railway domain
```

Then run one agent pass to seed the app from the device.
