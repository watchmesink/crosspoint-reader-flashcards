#!/usr/bin/env python3
"""CrossPoint <-> web flashcards sync agent.

Runs on a machine inside the same LAN as the Xteink X4 (the cloud app cannot
reach the device). One pass:

  1. Probe the device; exit quietly if offline.
  2. Per deck (german/ukrainian/english):
     - two-way file sync of /flashcards/<deck>/*.txt  (deletions propagate via
       the last-known sync state in ~/.crosspoint_sync/state.json)
     - progress sync: device bin -> web /api/sync/<deck>/progress -> merged bin
       written back to /.crosspoint/flashcards_<deck>.bin only when it changed.

Zero dependencies (urllib only). Use --watch to keep polling so a sync happens
every time the device joins the network. Idempotent: a pass with no changes
performs no writes on either side.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from typing import Dict, Optional, Tuple

DECKS = ("german", "ukrainian", "english")
DEFAULT_DEVICE = os.environ.get("CROSSPOINT_DEVICE", "")
DEFAULT_WEB = os.environ.get("CROSSPOINT_WEB", "")
DEFAULT_TOKEN = os.environ.get("CROSSPOINT_WEB_TOKEN", "")
STATE_PATH = os.path.expanduser(os.environ.get("CROSSPOINT_SYNC_STATE", "~/.crosspoint_sync/state.json"))


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---------- HTTP helpers ----------

def http(url: str, method: str = "GET", data: bytes | None = None,
         headers: Dict[str, str] | None = None, timeout: float = 20.0) -> Tuple[int, bytes]:
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.getcode(), resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read() if exc.fp else b""


class Device:
    def __init__(self, host: str, timeout: float = 15.0):
        self.host = host.rstrip("/")
        self.timeout = timeout

    def online(self) -> bool:
        try:
            code, _ = http(f"{self.host}/api/status", timeout=3.0)
            return code == 200
        except Exception:
            return False

    def list_dir(self, path: str) -> Optional[list]:
        code, body = http(f"{self.host}/api/files?{urllib.parse.urlencode({'path': path})}", timeout=self.timeout)
        if code != 200:
            return None
        try:
            return json.loads(body.decode("utf-8", "replace"))
        except json.JSONDecodeError:
            return None

    def download(self, path: str) -> Optional[bytes]:
        code, body = http(f"{self.host}/download?{urllib.parse.urlencode({'path': path})}", timeout=self.timeout)
        return body if code == 200 else None

    def mkdir_p(self, path: str) -> None:
        parent = "/"
        for seg in [s for s in path.split("/") if s]:
            code, body = http(
                f"{self.host}/mkdir", method="POST",
                data=urllib.parse.urlencode({"name": seg, "path": parent}).encode(),
                headers={"Content-Type": "application/x-www-form-urlencoded"},
                timeout=self.timeout,
            )
            if code != 200 and b"exists" not in body.lower():
                raise RuntimeError(f"device mkdir {seg} under {parent}: HTTP {code} {body[:120]!r}")
            parent = "/" + seg if parent == "/" else f"{parent}/{seg}"

    def upload(self, folder: str, name: str, content: bytes) -> None:
        boundary = f"----cpsync{uuid.uuid4().hex}"
        payload = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="file"; filename="{name}"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n"
        ).encode() + content + f"\r\n--{boundary}--\r\n".encode()
        url = f"{self.host}/upload?{urllib.parse.urlencode({'path': folder})}"
        code, body = http(url, method="POST", data=payload,
                          headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
                          timeout=max(self.timeout, 60.0))
        if not 200 <= code < 300:
            raise RuntimeError(f"device upload {folder}/{name}: HTTP {code} {body[:120]!r}")

    def delete(self, path: str) -> None:
        code, body = http(
            f"{self.host}/delete", method="POST",
            data=urllib.parse.urlencode({"path": path, "type": "file"}).encode(),
            headers={"Content-Type": "application/x-www-form-urlencoded"},
            timeout=self.timeout,
        )
        if code != 200:
            raise RuntimeError(f"device delete {path}: HTTP {code} {body[:120]!r}")


class Web:
    def __init__(self, base: str, token: str, timeout: float = 30.0):
        self.base = base.rstrip("/")
        self.token = token
        self.timeout = timeout

    def _headers(self, extra: Dict[str, str] | None = None) -> Dict[str, str]:
        h = dict(extra or {})
        if self.token:
            h["Authorization"] = f"Bearer {self.token}"
        return h

    def list_files(self, deck: str) -> list:
        code, body = http(f"{self.base}/api/decks/{deck}/files", headers=self._headers(), timeout=self.timeout)
        if code != 200:
            raise RuntimeError(f"web list {deck}: HTTP {code} {body[:120]!r}")
        return json.loads(body)

    def get_file(self, deck: str, name: str) -> bytes:
        code, body = http(f"{self.base}/api/decks/{deck}/files/{urllib.parse.quote(name)}",
                          headers=self._headers(), timeout=self.timeout)
        if code != 200:
            raise RuntimeError(f"web get {deck}/{name}: HTTP {code}")
        return body

    def put_file(self, deck: str, name: str, content: bytes) -> None:
        code, body = http(f"{self.base}/api/decks/{deck}/files/{urllib.parse.quote(name)}", method="PUT",
                          data=content, headers=self._headers({"Content-Type": "text/plain"}),
                          timeout=self.timeout)
        if code != 200:
            raise RuntimeError(f"web put {deck}/{name}: HTTP {code} {body[:120]!r}")

    def delete_file(self, deck: str, name: str) -> None:
        code, body = http(f"{self.base}/api/decks/{deck}/files/{urllib.parse.quote(name)}", method="DELETE",
                          headers=self._headers(), timeout=self.timeout)
        if code != 200:
            raise RuntimeError(f"web delete {deck}/{name}: HTTP {code} {body[:120]!r}")

    def sync_progress(self, deck: str, device_bin: Optional[bytes]) -> Optional[bytes]:
        payload = json.dumps({"bin": base64.b64encode(device_bin).decode() if device_bin else None}).encode()
        code, body = http(f"{self.base}/api/sync/{deck}/progress", method="POST", data=payload,
                          headers=self._headers({"Content-Type": "application/json"}), timeout=self.timeout)
        if code != 200:
            raise RuntimeError(f"web progress sync {deck}: HTTP {code} {body[:160]!r}")
        merged = json.loads(body).get("bin")
        return base64.b64decode(merged) if merged else None


# ---------- state ----------

def load_state() -> dict:
    try:
        with open(STATE_PATH, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, json.JSONDecodeError):
        return {}


def save_state(state: dict) -> None:
    os.makedirs(os.path.dirname(STATE_PATH), exist_ok=True)
    tmp = STATE_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(state, fh, indent=1)
    os.replace(tmp, STATE_PATH)


# ---------- sync logic ----------

def is_txt(name: str) -> bool:
    return name.lower().endswith(".txt") and not name.startswith(".")


def sync_deck_files(device: Device, web: Web, deck: str, deck_state: dict, dry_run: bool) -> dict:
    """Two-way file sync. Returns the converged {name: {size, sha}} map."""
    folder = f"/flashcards/{deck}"
    listing = device.list_dir(folder)
    dev_files: Dict[str, int] = {}
    if listing is not None:
        for entry in listing:
            if not entry.get("isDirectory") and is_txt(entry.get("name", "")):
                dev_files[entry["name"]] = int(entry.get("size", 0))

    web_files = {f["name"]: f for f in web.list_files(deck) if is_txt(f["name"])}
    prev: Dict[str, dict] = deck_state.get("files", {})
    converged: Dict[str, dict] = {}
    actions = []
    folder_ensured = listing is not None

    def ensure_folder():
        nonlocal folder_ensured
        if not folder_ensured and not dry_run:
            device.mkdir_p(folder)
            folder_ensured = True

    for name in sorted(set(dev_files) | set(web_files)):
        on_dev, on_web = name in dev_files, name in web_files
        known = prev.get(name)

        if on_dev and on_web:
            if known and known.get("size") == dev_files[name] and known.get("sha") == web_files[name]["sha256"]:
                converged[name] = known  # unchanged on both sides
                continue
            content = device.download(f"{folder}/{name}")
            if content is None:
                log(f"  ! could not download {folder}/{name}, skipping")
                continue
            dev_sha = sha256(content)
            if dev_sha != web_files[name]["sha256"]:
                actions.append(f"update web <- device: {name}")
                if not dry_run:
                    web.put_file(deck, name, content)
            converged[name] = {"size": len(content), "sha": dev_sha}

        elif on_dev and not on_web:
            if known:
                actions.append(f"delete on device (removed on web): {name}")
                if not dry_run:
                    device.delete(f"{folder}/{name}")
            else:
                content = device.download(f"{folder}/{name}")
                if content is None:
                    log(f"  ! could not download {folder}/{name}, skipping")
                    continue
                actions.append(f"upload to web: {name}")
                if not dry_run:
                    web.put_file(deck, name, content)
                converged[name] = {"size": len(content), "sha": sha256(content)}

        else:  # on web only
            if known:
                actions.append(f"delete on web (removed on device): {name}")
                if not dry_run:
                    web.delete_file(deck, name)
            else:
                content = web.get_file(deck, name)
                actions.append(f"upload to device: {name}")
                if not dry_run:
                    ensure_folder()
                    device.upload(folder, name, content)
                converged[name] = {"size": len(content), "sha": sha256(content)}

    for a in actions:
        log(f"  {deck}: {a}")
    if not actions:
        log(f"  {deck}: files in sync ({len(converged)})")
    return converged


def sync_deck_progress(device: Device, web: Web, deck: str, dry_run: bool) -> None:
    bin_path = f"/.crosspoint/flashcards_{deck}.bin"
    device_bin = device.download(bin_path)
    merged = web.sync_progress(deck, device_bin)
    if merged is None:
        log(f"  {deck}: no progress anywhere yet")
        return
    if device_bin == merged:
        log(f"  {deck}: progress in sync ({len(merged)} bytes)")
        return
    log(f"  {deck}: writing merged progress to device ({len(merged)} bytes, was "
        f"{len(device_bin) if device_bin else 0})")
    if not dry_run:
        device.mkdir_p("/.crosspoint")
        device.upload("/.crosspoint", f"flashcards_{deck}.bin", merged)


def sync_pass(device: Device, web: Web, dry_run: bool = False) -> bool:
    if not device.online():
        return False
    log(f"device online at {device.host}, syncing{' (dry run)' if dry_run else ''}")
    state = load_state()
    decks_state = state.setdefault("decks", {})
    for deck in DECKS:
        deck_state = decks_state.setdefault(deck, {})
        try:
            converged = sync_deck_files(device, web, deck, deck_state, dry_run)
            sync_deck_progress(device, web, deck, dry_run)
            if not dry_run:
                deck_state["files"] = converged
        except Exception as exc:  # keep other decks going
            log(f"  ! {deck}: {exc}")
    if not dry_run:
        state["lastSyncAt"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        save_state(state)
    log("sync pass complete")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Sync CrossPoint device flashcards with the web app.")
    parser.add_argument("--device", default=DEFAULT_DEVICE,
                        help="Device base URL, e.g. http://192.168.1.50 (or CROSSPOINT_DEVICE env)")
    parser.add_argument("--web", default=DEFAULT_WEB, help="Web app base URL (or CROSSPOINT_WEB env)")
    parser.add_argument("--token", default=DEFAULT_TOKEN, help="Web API token (or CROSSPOINT_WEB_TOKEN env)")
    parser.add_argument("--watch", action="store_true", help="Poll forever; sync whenever the device is online")
    parser.add_argument("--interval", type=float, default=30.0, help="Watch poll interval seconds (default 30)")
    parser.add_argument("--resync-every", type=float, default=600.0,
                        help="While device stays online, re-sync at most this often in seconds (default 600)")
    parser.add_argument("--dry-run", action="store_true", help="Report actions without writing")
    args = parser.parse_args()

    if not args.web:
        print("ERROR: --web URL (or CROSSPOINT_WEB) is required", file=sys.stderr)
        return 2
    if not args.device:
        print("ERROR: --device URL (or CROSSPOINT_DEVICE) is required, e.g. http://192.168.1.50", file=sys.stderr)
        return 2

    device = Device(args.device)
    web = Web(args.web, args.token)

    if not args.watch:
        synced = sync_pass(device, web, dry_run=args.dry_run)
        if not synced:
            log(f"device offline at {args.device}; nothing to do")
        return 0

    log(f"watching for {args.device} every {args.interval:.0f}s -> {args.web}")
    was_online = False
    last_sync = 0.0
    while True:
        online = device.online()
        try:
            if online and (not was_online or time.time() - last_sync >= args.resync_every):
                if sync_pass(device, web, dry_run=args.dry_run):
                    last_sync = time.time()
        except Exception as exc:
            log(f"! sync error: {exc}")
        was_online = online
        time.sleep(args.interval)


if __name__ == "__main__":
    raise SystemExit(main())
