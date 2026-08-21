# Hands-free Kindle → flashcards (no USB, no manual run)

This makes words you look up on the Kindle appear in your CrossPoint decks
**automatically and wirelessly** — you never plug the Kindle into a computer and
never run a script by hand.

## How it works

```
Kindle (you look up a word) ──▶ system/vocabulary/vocab.db (on the Kindle)
        │  cron on the Kindle runs upload_vocab.sh over WiFi
        ▼
POST /api/kindle/vocab   (web app: web/server.js)
        │  server reads new WORDS (sqlite3), translates, files them into decks
        ▼
decks/<lang>/kindle-YYYY-MM-DD.txt ──▶ web app + device (normal sync)
```

The upload is incremental on the server side (it tracks the newest word it has
already imported), so running it often is cheap and safe.

## ⚠️ Requirement: a jailbroken Kindle

Amazon keeps Vocabulary Builder (`vocab.db`) **local to the device** — it is not
exposed to any cloud or API. The only way to read it without a USB cable is to
run a tiny script **on the Kindle itself**, which requires a **jailbroken**
Kindle with KUAL. See https://kindlemodding.org/ for jailbreaking + KUAL.

(If you don't want to jailbreak: there is no way to get Vocabulary Builder words
off the device wirelessly. The nearest alternative is to *highlight* words while
reading — highlights do sync to Amazon's cloud — and scrape
`read.amazon.com/notebook` on a schedule, but that changes your reading gesture
and needs your Amazon session. Ask and I can build that instead.)

## One-time setup on the Kindle

1. **Jailbreak + install KUAL** (see kindlemodding.org). Disable OTA updates so
   an Amazon update doesn't undo the jailbreak.
2. Make sure **`curl`** (or BusyBox `wget` with POST support) is available. Most
   jailbroken setups have one; install via the community tools if not.
3. Copy `upload_vocab.sh` and a filled-in `vocab_sync.conf` (from
   `vocab_sync.conf.example`) to `/mnt/us/` on the Kindle:
   - set `WEB_URL` to your web app and `WEB_TOKEN` to its `API_TOKEN`.
   - protect the token with `chmod 600 /mnt/us/vocab_sync.conf`.
4. Make it executable and do a first manual test (over WiFi):
   ```sh
   sh /mnt/us/upload_vocab.sh && cat /mnt/us/vocab_sync.log
   ```
   You should see `uploaded ok: {"stored":true,...}` and the words should show up
   in the web app shortly.
5. **Schedule it with cron** so it runs on its own. Exact cron setup depends on
   your jailbreak/model; the common approach is a crontab entry that runs it
   periodically, e.g. hourly:
   ```
   0 * * * * /bin/sh /mnt/us/upload_vocab.sh
   ```
   On most jailbroken Kindles the built-in `crond` reads `/etc/crontab/root`
   (add the line there), or you can use a KUAL cron extension. See the
   kindlemodding.org docs and the community `kindle-dash-client` project for the
   exact cron pattern on your firmware. The script exits quietly when the Kindle
   is offline, so a periodic schedule "just works" — it uploads whenever WiFi is
   up. "Some lag is acceptable," so hourly (or even a few times a day) is fine.

## Server side (once)

Deploy the web app from this repo (it already includes the ingestion endpoint):
`POST /api/kindle/vocab` (auth via `Authorization: Bearer <API_TOKEN>`). It needs
the `sqlite3` CLI, which the included `web/nixpacks.toml` installs on Railway.
Check progress any time with `GET /api/kindle/status`.

## Files

| File | Runs where | What it does |
|---|---|---|
| `upload_vocab.sh` | on the Kindle | uploads `vocab.db` to the web app over WiFi |
| `vocab_sync.conf.example` | on the Kindle | template for `WEB_URL` / `WEB_TOKEN` |
| `web/server.js` `POST /api/kindle/vocab` | web app | parses, translates, files words into decks |
