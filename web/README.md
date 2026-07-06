# CrossPoint Flashcards Web

Mobile-first web app that mirrors the flashcards mode of the CrossPoint firmware
(Xteink X4 e-reader) and keeps decks + learning progress in two-way sync with the
device.

The scheduling engine (`engine.js`) is an exact port of
`src/activities/flashcards/FlashcardsActivity.cpp` (branch `codex/flashcards-0.1.13-push`):

- Decks: `german`, `ukrainian`, `english`; cards are `prompt<TAB>translation`
  lines in `.txt` files (`#` comments, fallback split on last comma).
- Card identity: FNV-1a hash of `prompt\tanswer` — progress survives file
  renames/moves and is what makes device<->web merge possible.
- SM-2++: learning steps {1, 8, 48} measured in *review steps* (not days),
  Hard/Good/Easy ratings, ease 1.30–3.00, interval cap 4096, deterministic
  interval fuzz, batches of 20 where only **Easy** marks a card processed.
- Progress binary: byte-compatible with `/.crosspoint/flashcards_<deck>.bin`
  (version 6) — the test suite round-trips a real device file byte-identically.

## Components

| Path | What it is |
|---|---|
| `server.js` | Zero-dependency Node server: REST API + static SPA + progress merge |
| `engine.js` | Firmware engine port + bin codec + merge |
| `public/` | Mobile SPA (deck list → deck → study/files), PWA manifest |
| `agent/sync_agent.py` | LAN-side sync agent (device ⇄ web), zero-dep Python |
| `test/test.js` | Engine tests incl. byte-identical round-trip of a real device bin |

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
  `decks/<deck>/files/*.txt` + `decks/<deck>/progress.json`.
- `PUBLIC_URL` — optional public app URL used in phone notifications.
- `PUSHOVER_API_TOKEN`, `PUSHOVER_USER_KEY`, `PUSHOVER_DEVICE` — optional
  server-side defaults for study notifications. They can also be saved from
  the web app's Notifications screen.

## Phone Study Notifications

The web app can notify your phone through Pushover when cards are due. Open the
app, go to **Notifications**, enable study notifications, enter the Pushover app
token and user key, then use **Send test**. **Send due** sends one reminder for
the current due cards.

Automatic reminders run only from the web app server:

- after deck files are uploaded, changed, or deleted in the web UI;
- after the device syncs progress into the web app;
- once per day at the configured daily time, if set.

The app de-duplicates the current due-card set so the same sync or file change
does not repeatedly notify your phone. The due calculation uses the same
review-step scheduler as the device, not wall-clock card due dates.

### API

- `GET /api/decks` — summaries (total, memorized, due, streak, last device sync)
- `GET /api/decks/:deck` — deck view incl. current card
- `POST /api/decks/:deck/rate` `{key, rating: hard|good|easy}` — rate + advance
- `GET/PUT/DELETE /api/decks/:deck/files[/:name]` — deck TXT files
- `GET/POST /api/notifications/settings` — Pushover reminder settings
- `POST /api/notifications/test` — send a test phone notification
- `POST /api/notifications/due` `{force?: true}` — send/check due-card reminder
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
