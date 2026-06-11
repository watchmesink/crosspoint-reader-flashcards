#pragma once

#include <string>

// Device-initiated flashcards sync with the companion web app.
//
// The device has no persistent WiFi: shortly after boot/wake this module
// briefly brings WiFi up itself (last saved network), runs one background sync
// pass, and powers the radio back off. If WiFi is already up for another
// reason (file transfer etc.) the pass piggybacks on that connection and
// leaves the radio alone. Configuration lives in
// /.crosspoint/flashcards_sync.json:
//
//   {"url": "https://your-app.example", "token": "<API_TOKEN>", "enabled": true}
//
// Per deck (german/ukrainian/english):
//   1. Progress: POST the local /.crosspoint/flashcards_<deck>.bin to
//      <url>/api/sync/<deck>/progress; the server merges it with web reviews
//      (SM-2++, more-reviewed record per card wins) and returns the merged bin,
//      which is written back only if it differs.
//   2. Files: reconcile /flashcards/<deck>/*.txt against
//      <url>/api/sync/<deck>/manifest — download web-only files, upload
//      device-only files, honor web deletion tombstones, and re-upload on
//      content mismatch (device wins).
//
// Without the config file this module is inert (and never touches the radio).
// Sync runs at most once per WiFi connection (retried after a delay if the
// pass had errors).
namespace FlashcardsWebSync {

// Cheap state machine tick; call from the main loop.
void loopTick();

// True while the background sync task is running (used to hold off auto-sleep).
bool isSyncing();

// Short human-readable outcome of the last pass, e.g. "synced 3 decks".
std::string lastSummary();

}  // namespace FlashcardsWebSync
