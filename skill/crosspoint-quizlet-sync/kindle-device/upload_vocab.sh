#!/bin/sh
# Upload the Kindle Vocabulary Builder database to the CrossPoint flashcards web
# app over WiFi. Runs ON a jailbroken Kindle (BusyBox sh) — no USB, no computer.
# The web app ingests vocab.db (POST /api/kindle/vocab): parses new WORDS,
# translates them, and files them into the decks. Safe to run repeatedly; the
# server only ingests words newer than its last watermark.
#
# Config: put WEB_URL and WEB_TOKEN in a vocab_sync.conf next to this script or
# at /mnt/us/vocab_sync.conf (see vocab_sync.conf.example). Then run this from
# cron (see README.md). It exits quietly when offline; cron retries later.

SELF_DIR=$(dirname "$0")
for CONF in "${VOCAB_SYNC_CONF:-}" "$SELF_DIR/vocab_sync.conf" "/mnt/us/vocab_sync.conf"; do
  [ -n "$CONF" ] && [ -f "$CONF" ] && . "$CONF" && break
done

DB="${VOCAB_DB:-/mnt/us/system/vocabulary/vocab.db}"
WEB="${WEB_URL:-}"
TOKEN="${WEB_TOKEN:-}"
LOG="${LOG_FILE:-/mnt/us/vocab_sync.log}"
TIMEOUT="${HTTP_TIMEOUT:-120}"

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG" 2>/dev/null; }

[ -z "$WEB" ] && { log "ERROR: WEB_URL not configured"; exit 1; }
[ -f "$DB" ] || { log "ERROR: vocab.db not found at $DB"; exit 1; }

URL="$WEB/api/kindle/vocab"

# Prefer curl; fall back to BusyBox wget. Either uploads the raw DB as the body.
if command -v curl >/dev/null 2>&1; then
  OUT=$(curl -s -m "$TIMEOUT" -H "Authorization: Bearer $TOKEN" \
        --data-binary @"$DB" -X POST "$URL" 2>>"$LOG")
  RC=$?
elif command -v wget >/dev/null 2>&1; then
  OUT=$(wget -q -O - --timeout="$TIMEOUT" \
        --header="Authorization: Bearer $TOKEN" --post-file="$DB" "$URL" 2>>"$LOG")
  RC=$?
else
  log "ERROR: neither curl nor wget is available on this Kindle"
  exit 1
fi

if [ "$RC" -eq 0 ]; then
  log "uploaded ok: $OUT"
else
  log "upload failed (rc=$RC) — offline or server unreachable; will retry"
fi
exit "$RC"
