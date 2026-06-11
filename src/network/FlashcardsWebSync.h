#pragma once

#include <string>

// Device-initiated flashcards sync with the companion web app.
//
// When WiFi comes up (any path: boot auto-connect, settings, web server
// activity), one background sync pass runs against the web app configured in
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
// Without the config file this module is inert. Sync runs at most once per
// WiFi connection (retried after a delay if the pass had errors).
namespace FlashcardsWebSync {

// Cheap state machine tick; call from the main loop.
void loopTick();

// True while the background sync task is running (used to hold off auto-sleep).
bool isSyncing();

// Short human-readable outcome of the last pass, e.g. "synced 3 decks".
std::string lastSummary();

}  // namespace FlashcardsWebSync
