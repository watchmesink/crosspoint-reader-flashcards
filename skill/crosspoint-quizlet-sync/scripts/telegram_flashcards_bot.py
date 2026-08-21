#!/usr/bin/env python3
"""Telegram bot that stores vocab and exports explicit pairs as Markdown."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, List

from quizlet_sync import collect_pairs_by_deck, render_markdown_export


STATE_VERSION = 2


def _now_iso() -> str:
    return dt.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"


def _telegram_request(token: str, method: str, data: Dict[str, object]) -> dict:
    encoded = urllib.parse.urlencode({key: str(value) for key, value in data.items()}).encode("utf-8")
    request = urllib.request.Request(
        url=f"https://api.telegram.org/bot{token}/{method}",
        data=encoded,
        method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        payload = response.read().decode("utf-8", errors="replace")
    result = json.loads(payload)
    if not result.get("ok"):
        raise RuntimeError(f"Telegram API {method} failed: {payload}")
    return result


def _telegram_request_multipart(
    token: str,
    method: str,
    fields: Dict[str, object],
    *,
    file_field: str,
    file_name: str,
    content: bytes,
    content_type: str,
) -> dict:
    boundary = f"----telegram-{uuid.uuid4().hex}"
    chunks: List[bytes] = []

    for key, value in fields.items():
        chunks.append(f"--{boundary}\r\n".encode("utf-8"))
        chunks.append(f'Content-Disposition: form-data; name="{key}"\r\n\r\n'.encode("utf-8"))
        chunks.append(str(value).encode("utf-8"))
        chunks.append(b"\r\n")

    chunks.append(
        (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="{file_field}"; filename="{file_name}"\r\n'
            f"Content-Type: {content_type}\r\n\r\n"
        ).encode("utf-8")
    )
    chunks.append(content)
    chunks.append(b"\r\n")
    chunks.append(f"--{boundary}--\r\n".encode("utf-8"))
    payload = b"".join(chunks)

    request = urllib.request.Request(
        url=f"https://api.telegram.org/bot{token}/{method}",
        data=payload,
        method="POST",
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(payload)),
        },
    )
    with urllib.request.urlopen(request, timeout=120) as response:
        raw = response.read().decode("utf-8", errors="replace")

    result = json.loads(raw)
    if not result.get("ok"):
        raise RuntimeError(f"Telegram API {method} failed: {raw}")
    return result


def _send_message(token: str, chat_id: int, text: str) -> None:
    _telegram_request(token, "sendMessage", {"chat_id": chat_id, "text": text})


def _send_document(token: str, chat_id: int, file_name: str, content: bytes, caption: str | None = None) -> None:
    fields: Dict[str, object] = {"chat_id": chat_id}
    if caption:
        fields["caption"] = caption
    _telegram_request_multipart(
        token,
        "sendDocument",
        fields,
        file_field="document",
        file_name=file_name,
        content=content,
        content_type="text/markdown; charset=utf-8",
    )


def _split_submission(text: str) -> List[str]:
    items: List[str] = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if "\t" in line or "::" in line:
            items.append(line)
            continue
        if line.count(",") > 1:
            items.extend(part.strip() for part in line.split(",") if part.strip())
            continue
        items.append(line)
    return items


def _parse_command(text: str) -> str | None:
    stripped = text.strip()
    if not stripped.startswith("/"):
        return None

    first = stripped.split(None, 1)[0][1:].lower()
    if "@" in first:
        first = first.split("@", 1)[0]
    return first or None


def _chat_meta(message: Dict[str, Any]) -> Dict[str, Any]:
    chat = message.get("chat") or {}
    return {
        "id": chat.get("id"),
        "type": chat.get("type"),
        "first_name": chat.get("first_name"),
        "last_name": chat.get("last_name"),
        "username": chat.get("username"),
    }


class StateStore:
    def __init__(self, path: Path):
        self.path = path
        self.lock = threading.Lock()
        self.state = self._load()

    def _default_state(self) -> Dict[str, Any]:
        return {"version": STATE_VERSION, "offset": 0, "chats": {}}

    def _load(self) -> Dict[str, Any]:
        if not self.path.exists():
            return self._default_state()
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return self._default_state()

        if not isinstance(data, dict):
            return self._default_state()

        data.setdefault("version", STATE_VERSION)
        data.setdefault("offset", 0)
        data.setdefault("chats", {})
        if not isinstance(data["chats"], dict):
            data["chats"] = {}
        return data

    def _save_locked(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temp_path = self.path.with_suffix(".tmp")
        temp_path.write_text(json.dumps(self.state, ensure_ascii=False, indent=2), encoding="utf-8")
        temp_path.replace(self.path)

    def _ensure_chat_locked(self, chat_id: int, chat: Dict[str, Any] | None = None) -> Dict[str, Any]:
        key = str(chat_id)
        chats = self.state["chats"]
        if key not in chats:
            chats[key] = {
                "chat": chat or {"id": chat_id},
                "items": [],
                "export_requested": False,
                "export_requested_at": None,
                "last_export_at": None,
                "last_export_summary": None,
                "last_export_error": None,
                "updated_at": _now_iso(),
            }
        elif chat:
            chats[key]["chat"] = {**chats[key].get("chat", {}), **chat}
        return chats[key]

    def get_offset(self) -> int:
        with self.lock:
            return int(self.state.get("offset", 0))

    def set_offset(self, offset: int) -> None:
        with self.lock:
            self.state["offset"] = int(offset)
            self._save_locked()

    def queue_items(self, chat_id: int, chat: Dict[str, Any], items: List[str]) -> int:
        with self.lock:
            chat_state = self._ensure_chat_locked(chat_id, chat)
            chat_state["items"].extend(items)
            chat_state["updated_at"] = _now_iso()
            self._save_locked()
            return len(chat_state["items"])

    def clear_chat(self, chat_id: int, chat: Dict[str, Any]) -> None:
        with self.lock:
            chat_state = self._ensure_chat_locked(chat_id, chat)
            chat_state["items"] = []
            chat_state["export_requested"] = False
            chat_state["export_requested_at"] = None
            chat_state["last_export_error"] = None
            chat_state["updated_at"] = _now_iso()
            self._save_locked()

    def get_chat_status(self, chat_id: int, chat: Dict[str, Any] | None = None) -> Dict[str, Any]:
        with self.lock:
            if chat is not None:
                self._ensure_chat_locked(chat_id, chat)
                self._save_locked()
            return self._snapshot_chat_locked(chat_id)

    def _snapshot_chat_locked(self, chat_id: int) -> Dict[str, Any]:
        chat_state = self.state["chats"].get(str(chat_id), {})
        items = list(chat_state.get("items", []))
        return {
            "chat_id": int(chat_id),
            "chat": dict(chat_state.get("chat", {})),
            "pending_count": len(items),
            "items": items,
            "last_export_at": chat_state.get("last_export_at"),
            "last_export_summary": chat_state.get("last_export_summary"),
            "last_export_error": chat_state.get("last_export_error"),
            "updated_at": chat_state.get("updated_at"),
        }

    def mark_export_complete(self, chat_id: int, summary: Dict[str, Any]) -> Dict[str, Any]:
        with self.lock:
            chat_state = self._ensure_chat_locked(chat_id)
            chat_state["items"] = []
            chat_state["export_requested"] = False
            chat_state["export_requested_at"] = None
            chat_state["last_export_at"] = _now_iso()
            chat_state["last_export_summary"] = summary
            chat_state["last_export_error"] = None
            chat_state["updated_at"] = _now_iso()
            self._save_locked()
            return self._snapshot_chat_locked(chat_id)

    def mark_export_failed(self, chat_id: int, error: str) -> Dict[str, Any]:
        with self.lock:
            chat_state = self._ensure_chat_locked(chat_id)
            chat_state["last_export_error"] = error
            chat_state["updated_at"] = _now_iso()
            self._save_locked()
            return self._snapshot_chat_locked(chat_id)


class BotService:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.allowed_chat_ids = {str(chat_id) for chat_id in args.allowed_chat_id}
        self.state = StateStore(Path(args.state_dir) / "state.json")

    def queue_items(self, message: Dict[str, Any], items: List[str]) -> int:
        return self.state.queue_items(int(message["chat"]["id"]), _chat_meta(message), items)

    def clear_chat(self, message: Dict[str, Any]) -> None:
        self.state.clear_chat(int(message["chat"]["id"]), _chat_meta(message))

    def chat_status(self, message: Dict[str, Any]) -> Dict[str, Any]:
        return self.state.get_chat_status(int(message["chat"]["id"]), _chat_meta(message))

    def send_message(self, chat_id: int, text: str) -> None:
        _send_message(self.args.token, chat_id, text)

    def send_document(self, chat_id: int, file_name: str, content: bytes, caption: str | None = None) -> None:
        _send_document(self.args.token, chat_id, file_name, content, caption=caption)

    def format_pending(self, status: Dict[str, Any]) -> str:
        lines = [f"Pending items: {status['pending_count']}"]
        summary = status.get("last_export_summary") or {}
        if status.get("last_export_at"):
            lines.append(f"Last export: {status['last_export_at']}")
        if isinstance(summary, dict) and summary.get("file_name"):
            lines.append(f"Last file: {summary['file_name']}")
        if status.get("last_export_error"):
            lines.append(f"Last export error: {status['last_export_error']}")
        return "\n".join(lines)

    def format_export_complete(self, summary: Dict[str, Any]) -> str:
        results = summary.get("results", [])
        total_cards = sum(int(result.get("cards", 0)) for result in results)
        file_name = summary.get("file_name", "flashcards-export.md")
        lines = [f"Exported {total_cards} cards to {file_name}. Queue cleared."]
        for result in results:
            deck = str(result.get("deck", "")).capitalize()
            cards = int(result.get("cards", 0))
            lines.append(f"{deck}: {cards} cards")
        if summary.get("skipped"):
            lines.append(f"Skipped lines: {summary['skipped']}")
        if summary.get("ambiguous_lines"):
            lines.append(f"Ambiguous lines routed: {summary['ambiguous_lines']}")
        if summary.get("translation_errors"):
            lines.append(f"Translation failures: {summary['translation_errors']}")
        return "\n".join(lines)

    def _build_export_file_name(self) -> str:
        timestamp = dt.datetime.utcnow().strftime("%Y-%m-%d-%H%M%S")
        return f"{self.args.export_prefix}-{timestamp}.md"

    def _export_summary_payload(self, summary: Any, pairs_by_deck: Dict[str, List[Any]], file_name: str) -> Dict[str, Any]:
        return {
            "file_name": file_name,
            "results": [
                {"deck": deck_key, "cards": len(pairs)}
                for deck_key, pairs in pairs_by_deck.items()
                if pairs
            ],
            "skipped": summary.skipped,
            "translated_count": summary.translated_count,
            "translation_errors": summary.translation_errors,
            "ambiguous_lines": summary.ambiguous_lines,
            "unrouted_lines": summary.unrouted_lines,
        }

    def export_queue(self, message: Dict[str, Any]) -> None:
        chat_id = int(message["chat"]["id"])
        status = self.chat_status(message)
        if status["pending_count"] == 0:
            self.send_message(chat_id, "No queued items. Send words first, then use /export.")
            return

        try:
            pairs_by_deck, summary = collect_pairs_by_deck(
                status["items"],
                timeout_sec=self.args.translation_timeout,
                source_lang_override=self.args.source_lang,
                target_lang_override=self.args.target_lang,
                auto_translate=True,
                forced_deck=None,
                ambiguous_policy=self.args.ambiguous_policy,
            )
            total_cards = sum(len(pairs) for pairs in pairs_by_deck.values())
            if total_cards == 0:
                raise RuntimeError("No valid term/translation pairs could be produced from the queue.")

            file_name = self._build_export_file_name()
            markdown = render_markdown_export(pairs_by_deck, summary)
            self.send_document(
                chat_id,
                file_name,
                markdown.encode("utf-8"),
                caption=f"Flashcards export: {total_cards} cards.",
            )
            summary_payload = self._export_summary_payload(summary, pairs_by_deck, file_name)
            self.state.mark_export_complete(chat_id, summary_payload)
            self.send_message(chat_id, self.format_export_complete(summary_payload))
        except Exception as exc:
            failed = self.state.mark_export_failed(chat_id, str(exc))
            self.send_message(chat_id, f"Export failed: {exc}\nQueue kept: {failed['pending_count']} items.")


def _show_help(service: BotService, chat_id: int) -> None:
    service.send_message(
        chat_id,
        "\n".join(
            [
                "Send words or explicit term/translation pairs and I will store them.",
                "Commands:",
                "/pending - show queued items and last export status",
                "/export - build explicit pairs and send a Markdown file",
                "/clear - clear your pending queue",
            ]
        ),
    )


def _run_polling(service: BotService) -> None:
    while True:
        try:
            response = _telegram_request(
                service.args.token,
                "getUpdates",
                {"offset": service.state.get_offset(), "timeout": service.args.poll_timeout},
            )
            updates = response.get("result", [])
        except (urllib.error.URLError, TimeoutError, RuntimeError) as exc:
            print(f"[warn] Telegram polling failed: {exc}", file=sys.stderr)
            time.sleep(3)
            continue

        for update in updates:
            next_offset = int(update["update_id"]) + 1
            if next_offset > service.state.get_offset():
                service.state.set_offset(next_offset)

            message = update.get("message")
            if not message:
                continue

            chat_id = (message.get("chat") or {}).get("id")
            text = str(message.get("text", "")).strip()
            if chat_id is None:
                continue

            if service.allowed_chat_ids and str(chat_id) not in service.allowed_chat_ids:
                continue

            if not text:
                service.send_message(chat_id, "Send text lines with words or pairs.")
                continue

            command = _parse_command(text)
            if command in {"start", "help"}:
                _show_help(service, chat_id)
                continue

            if command == "pending":
                service.send_message(chat_id, service.format_pending(service.chat_status(message)))
                continue

            if command == "clear":
                service.clear_chat(message)
                service.send_message(chat_id, "Cleared your pending queue.")
                continue

            if command == "export":
                service.export_queue(message)
                continue

            if command == "upload":
                service.send_message(chat_id, "Use /export. The bot now sends a Markdown file instead of uploading.")
                continue

            if command is not None:
                service.send_message(chat_id, "Unknown command. Use /help.")
                continue

            items = _split_submission(text)
            if not items:
                service.send_message(chat_id, "No usable words found in that message.")
                continue

            pending_count = service.queue_items(message, items)
            service.send_message(chat_id, f"Queued {len(items)} item(s). Pending: {pending_count}.")


class APIServer(BaseHTTPRequestHandler):
    server_version = "FlashcardsBot/2.0"

    def log_message(self, format: str, *args: Any) -> None:  # noqa: A003
        sys.stderr.write("%s - - [%s] %s\n" % (self.address_string(), self.log_date_time_string(), format % args))

    def _write_json(self, status: HTTPStatus, payload: Dict[str, Any]) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path in {"/", "/health"}:
            self._write_json(HTTPStatus.OK, {"ok": True, "time": _now_iso()})
            return
        self._write_json(HTTPStatus.NOT_FOUND, {"error": "not found"})


def main() -> int:
    env_allowed_chat_ids = [value.strip() for value in os.environ.get("ALLOWED_CHAT_IDS", "").split(",") if value.strip()]
    single_allowed_chat_id = os.environ.get("ALLOWED_CHAT_ID", "").strip()
    if single_allowed_chat_id:
        env_allowed_chat_ids.append(single_allowed_chat_id)

    parser = argparse.ArgumentParser(description="Telegram bot for CrossPoint flashcard markdown exports.")
    parser.add_argument("--token", default=os.environ.get("TELEGRAM_BOT_TOKEN"), help="Telegram bot token.")
    parser.add_argument(
        "--state-dir",
        default=os.path.expanduser(os.environ.get("FLASHCARDS_BOT_STATE_DIR", "~/.crosspoint_sync/telegram")),
        help="Directory for persistent state.",
    )
    parser.add_argument(
        "--allowed-chat-id",
        action="append",
        default=env_allowed_chat_ids,
        help="Optional allowlist of chat IDs. Repeat the flag to allow multiple chats.",
    )
    parser.add_argument(
        "--poll-timeout",
        type=int,
        default=int(os.environ.get("FLASHCARDS_BOT_POLL_TIMEOUT", "30")),
        help="Telegram long-poll timeout in seconds.",
    )
    parser.add_argument(
        "--ambiguous-policy",
        choices=("multi", "skip", "first"),
        default=os.environ.get("FLASHCARDS_BOT_AMBIGUOUS_POLICY", "multi"),
        help="What to do when deck language is ambiguous. Default: multi",
    )
    parser.add_argument("--source-lang", default=os.environ.get("FLASHCARDS_BOT_SOURCE_LANG"))
    parser.add_argument("--target-lang", default=os.environ.get("FLASHCARDS_BOT_TARGET_LANG"))
    parser.add_argument(
        "--translation-timeout",
        type=int,
        default=int(os.environ.get("FLASHCARDS_BOT_TRANSLATION_TIMEOUT", "20")),
        help="HTTP timeout for translation requests.",
    )
    parser.add_argument(
        "--export-prefix",
        default=os.environ.get("FLASHCARDS_BOT_EXPORT_PREFIX", "flashcards-export"),
        help="Filename prefix for exported markdown files.",
    )
    parser.add_argument("--host", default="0.0.0.0", help="HTTP bind host. Default: 0.0.0.0")
    parser.add_argument("--port", type=int, default=int(os.environ.get("PORT", "8080")), help="HTTP bind port.")
    args = parser.parse_args()

    if not args.token:
        print("ERROR: Provide --token or set TELEGRAM_BOT_TOKEN.", file=sys.stderr)
        return 2

    service = BotService(args)

    polling_thread = threading.Thread(target=_run_polling, args=(service,), daemon=True, name="telegram-polling")
    polling_thread.start()

    httpd = ThreadingHTTPServer((args.host, args.port), APIServer)
    print(f"[ok] HTTP health listening on {args.host}:{args.port}")
    print("[ok] Telegram polling started")
    httpd.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
