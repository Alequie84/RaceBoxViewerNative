#!/usr/bin/env python3
"""Durable, bounded storage for RaceBox Crew Chief companion chat rooms."""

from __future__ import annotations

import hashlib
import json
import secrets
import sqlite3
import time
import uuid
from pathlib import Path
from typing import Any, Callable


COMPANION_CONTRACT = "racebox-companion-session-v1"
MAX_MESSAGES_PER_SESSION = 2_000
MAX_MESSAGE_CHARS = 4_000
PAIRING_LIFETIME_SECONDS = 5 * 60


def _token_hash(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"))


def _plain_text(value: Any, limit: int) -> str:
    return " ".join(str(value or "").split()).strip()[:limit]


def _report_message_content(report: dict[str, Any]) -> str:
    """Turn the structured Crew Chief result into one useful shared chat message."""

    body = report.get("report", {})
    if not isinstance(body, dict):
        return ""
    summary = _plain_text(body.get("summary"), 1_000)
    if not summary:
        return ""
    sections = [summary]

    observations: list[str] = []
    supplied_observations = body.get("observations", [])
    if isinstance(supplied_observations, list):
        for item in supplied_observations[:2]:
            if not isinstance(item, dict):
                continue
            area = _plain_text(item.get("area"), 70)
            change = _plain_text(item.get("change"), 220)
            meaning = _plain_text(item.get("meaning"), 180)
            detail = ": ".join(part for part in (area, change) if part)
            if not detail and meaning:
                detail = meaning
            if detail:
                observations.append(f"- {detail}")
    if observations:
        sections.append("Key evidence:\n" + "\n".join(observations))

    confounds: list[str] = []
    supplied_confounds = body.get("confounds", [])
    if isinstance(supplied_confounds, list):
        confounds = [
            _plain_text(item, 220)
            for item in supplied_confounds[:1]
            if _plain_text(item, 220)
        ]
    if confounds:
        sections.append("Main uncertainty: " + confounds[0])

    next_test = _plain_text(body.get("next_test"), 400)
    if next_test:
        sections.append(f"Next test: {next_test}")
    return "\n\n".join(sections)[:MAX_MESSAGE_CHARS].rstrip()


class _ClosingConnection(sqlite3.Connection):
    def __exit__(self, exc_type, exc_value, traceback):
        try:
            return super().__exit__(exc_type, exc_value, traceback)
        finally:
            self.close()


class CompanionStore:
    def __init__(
        self,
        database_path: Path,
        backup_directory: Path,
        *,
        clock: Callable[[], float] = time.time,
    ) -> None:
        self.database_path = Path(database_path)
        self.backup_directory = Path(backup_directory)
        self.clock = clock
        self.database_path.parent.mkdir(parents=True, exist_ok=True)
        self.backup_directory.mkdir(parents=True, exist_ok=True)
        self._initialize()

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(
            self.database_path, timeout=30.0, factory=_ClosingConnection
        )
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute("PRAGMA journal_mode = WAL")
        connection.execute("PRAGMA busy_timeout = 30000")
        return connection

    def _initialize(self) -> None:
        with self._connect() as connection:
            connection.executescript(
                """
                CREATE TABLE IF NOT EXISTS sessions (
                    id TEXT PRIMARY KEY,
                    title TEXT NOT NULL,
                    context_json TEXT NOT NULL,
                    review_json TEXT NOT NULL,
                    prior_results_json TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL
                );
                CREATE TABLE IF NOT EXISTS session_tokens (
                    id TEXT PRIMARY KEY,
                    session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
                    token_hash TEXT NOT NULL UNIQUE,
                    kind TEXT NOT NULL CHECK(kind IN ('viewer', 'device')),
                    device_name TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    revoked_at INTEGER
                );
                CREATE TABLE IF NOT EXISTS pairings (
                    id TEXT PRIMARY KEY,
                    session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
                    code_hash TEXT NOT NULL UNIQUE,
                    expires_at INTEGER NOT NULL,
                    used_at INTEGER
                );
                CREATE TABLE IF NOT EXISTS messages (
                    cursor INTEGER PRIMARY KEY AUTOINCREMENT,
                    id TEXT NOT NULL UNIQUE,
                    session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
                    role TEXT NOT NULL CHECK(role IN ('user', 'assistant', 'system')),
                    content TEXT NOT NULL,
                    origin TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    report_json TEXT
                );
                CREATE INDEX IF NOT EXISTS messages_session_cursor
                    ON messages(session_id, cursor);
                CREATE TABLE IF NOT EXISTS jobs (
                    id TEXT PRIMARY KEY,
                    session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
                    user_message_id TEXT NOT NULL REFERENCES messages(id),
                    status TEXT NOT NULL,
                    assistant_message_id TEXT,
                    error TEXT NOT NULL DEFAULT '',
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL
                );
                CREATE UNIQUE INDEX IF NOT EXISTS jobs_user_message
                    ON jobs(session_id, user_message_id);
                """
            )

    def create_session(
        self,
        *,
        title: str,
        context: dict[str, Any],
        review: dict[str, Any],
        prior_results: list[dict[str, Any]],
        history: list[dict[str, Any]],
        public_gateway: str,
    ) -> dict[str, Any]:
        now = int(self.clock())
        session_id = uuid.uuid4().hex
        viewer_token = secrets.token_urlsafe(32)
        viewer_token_id = uuid.uuid4().hex
        pairing_id = uuid.uuid4().hex
        pairing_code = f"{secrets.randbelow(1_000_000):06d}"
        with self._connect() as connection:
            connection.execute(
                "INSERT INTO sessions VALUES (?, ?, ?, ?, ?, ?, ?)",
                (
                    session_id,
                    str(title).strip()[:200] or "RaceBox session",
                    _json(context),
                    _json(review),
                    _json(prior_results[:8]),
                    now,
                    now,
                ),
            )
            connection.execute(
                "INSERT INTO session_tokens VALUES (?, ?, ?, 'viewer', ?, ?, NULL)",
                (
                    viewer_token_id,
                    session_id,
                    _token_hash(viewer_token),
                    "RaceBox Viewer",
                    now,
                ),
            )
            connection.execute(
                "INSERT INTO pairings VALUES (?, ?, ?, ?, NULL)",
                (
                    pairing_id,
                    session_id,
                    _token_hash(pairing_code),
                    now + PAIRING_LIFETIME_SECONDS,
                ),
            )
            for supplied in history[-MAX_MESSAGES_PER_SESSION:]:
                if not isinstance(supplied, dict):
                    continue
                content = str(supplied.get("content") or "").strip()[:MAX_MESSAGE_CHARS]
                if not content:
                    continue
                role = str(supplied.get("role") or "user")
                if role not in {"user", "assistant", "system"}:
                    role = "user"
                message_id = str(supplied.get("id") or uuid.uuid4().hex)[:120]
                created_at = int(supplied.get("created_at") or now)
                origin = str(supplied.get("origin") or "viewer")[:80]
                connection.execute(
                    """INSERT OR IGNORE INTO messages
                       (id, session_id, role, content, origin, created_at, report_json)
                       VALUES (?, ?, ?, ?, ?, ?, NULL)""",
                    (message_id, session_id, role, content, origin, created_at),
                )
        pair_uri = (
            "raceboxcc://pair?gateway="
            + public_gateway.rstrip("/")
            + "&code="
            + pairing_code
        )
        return {
            "contract": COMPANION_CONTRACT,
            "session_id": session_id,
            "viewer_token": viewer_token,
            "pairing_id": pairing_id,
            "pairing_code": pairing_code,
            "pairing_expires_at": now + PAIRING_LIFETIME_SECONDS,
            "pair_uri": pair_uri,
        }

    def pair_device(self, code: str, device_name: str) -> dict[str, Any]:
        normalized = "".join(character for character in str(code) if character.isdigit())
        if len(normalized) != 6:
            raise ValueError("pairing code must contain six digits")
        now = int(self.clock())
        token = secrets.token_urlsafe(32)
        token_id = uuid.uuid4().hex
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            pairing = connection.execute(
                """SELECT id, session_id, expires_at, used_at FROM pairings
                   WHERE code_hash = ?""",
                (_token_hash(normalized),),
            ).fetchone()
            if pairing is None:
                raise ValueError("pairing code was not found")
            if pairing["used_at"] is not None:
                raise ValueError("pairing code was already used")
            if int(pairing["expires_at"]) < now:
                raise ValueError("pairing code expired")
            updated = connection.execute(
                "UPDATE pairings SET used_at = ? WHERE id = ? AND used_at IS NULL",
                (now, pairing["id"]),
            )
            if updated.rowcount != 1:
                raise ValueError("pairing code was already used")
            connection.execute(
                "INSERT INTO session_tokens VALUES (?, ?, ?, 'device', ?, ?, NULL)",
                (
                    token_id,
                    pairing["session_id"],
                    _token_hash(token),
                    str(device_name).strip()[:120] or "Android phone",
                    now,
                ),
            )
            title = connection.execute(
                "SELECT title FROM sessions WHERE id = ?", (pairing["session_id"],)
            ).fetchone()["title"]
        return {
            "contract": COMPANION_CONTRACT,
            "session_id": pairing["session_id"],
            "pairing_id": token_id,
            "session_token": token,
            "title": title,
        }

    def authenticate(
        self,
        session_id: str,
        token: str,
        *,
        allowed_kinds: tuple[str, ...] = ("viewer", "device"),
    ) -> dict[str, str] | None:
        if not token:
            return None
        with self._connect() as connection:
            row = connection.execute(
                """SELECT id, kind, device_name FROM session_tokens
                   WHERE session_id = ? AND token_hash = ? AND revoked_at IS NULL""",
                (session_id, _token_hash(token)),
            ).fetchone()
        if row is None or row["kind"] not in allowed_kinds:
            return None
        return {"id": row["id"], "kind": row["kind"], "device_name": row["device_name"]}

    def revoke_pairing(self, pairing_id: str, session_id: str) -> bool:
        now = int(self.clock())
        with self._connect() as connection:
            updated = connection.execute(
                """UPDATE session_tokens SET revoked_at = ?
                   WHERE id = ? AND session_id = ? AND kind = 'device' AND revoked_at IS NULL""",
                (now, pairing_id, session_id),
            )
        return updated.rowcount == 1

    def get_session(self, session_id: str) -> dict[str, Any]:
        with self._connect() as connection:
            row = connection.execute(
                "SELECT * FROM sessions WHERE id = ?", (session_id,)
            ).fetchone()
        if row is None:
            raise KeyError("companion session was not found")
        return {
            "id": row["id"],
            "title": row["title"],
            "context": json.loads(row["context_json"]),
            "review": json.loads(row["review_json"]),
            "prior_results": json.loads(row["prior_results_json"]),
            "created_at": row["created_at"],
            "updated_at": row["updated_at"],
        }

    def list_messages(
        self, session_id: str, after: int = 0, *, limit: int = 500
    ) -> dict[str, Any]:
        limit = max(1, min(MAX_MESSAGES_PER_SESSION, int(limit)))
        with self._connect() as connection:
            rows = connection.execute(
                """SELECT cursor, id, role, content, origin, created_at, report_json
                   FROM messages WHERE session_id = ? AND cursor > ?
                   ORDER BY cursor ASC LIMIT ?""",
                (session_id, max(0, int(after)), limit),
            ).fetchall()
            total = connection.execute(
                "SELECT COUNT(*) AS count FROM messages WHERE session_id = ?",
                (session_id,),
            ).fetchone()["count"]
        messages = []
        next_cursor = max(0, int(after))
        for row in rows:
            next_cursor = int(row["cursor"])
            messages.append(
                {
                    "cursor": next_cursor,
                    "id": row["id"],
                    "role": row["role"],
                    "content": row["content"],
                    "origin": row["origin"],
                    "created_at": row["created_at"],
                    "report": json.loads(row["report_json"]) if row["report_json"] else None,
                }
            )
        return {"messages": messages, "next_cursor": next_cursor, "total": int(total)}

    def create_message_job(
        self,
        session_id: str,
        *,
        message_id: str,
        content: str,
        origin: str,
    ) -> dict[str, Any]:
        message_id = str(message_id).strip()[:120]
        content = str(content).strip()[:MAX_MESSAGE_CHARS]
        if not message_id:
            raise ValueError("message_id is required")
        if not content:
            raise ValueError("message text is required")
        now = int(self.clock())
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            existing = connection.execute(
                "SELECT id, status FROM jobs WHERE session_id = ? AND user_message_id = ?",
                (session_id, message_id),
            ).fetchone()
            if existing is not None:
                return {"job_id": existing["id"], "status": existing["status"], "duplicate": True}
            count = connection.execute(
                "SELECT COUNT(*) AS count FROM messages WHERE session_id = ?",
                (session_id,),
            ).fetchone()["count"]
            if int(count) >= MAX_MESSAGES_PER_SESSION:
                raise OverflowError("conversation reached the 2000-message safety limit; export or clear it before continuing")
            session = connection.execute(
                "SELECT id FROM sessions WHERE id = ?", (session_id,)
            ).fetchone()
            if session is None:
                raise KeyError("companion session was not found")
            connection.execute(
                """INSERT INTO messages
                   (id, session_id, role, content, origin, created_at, report_json)
                   VALUES (?, ?, 'user', ?, ?, ?, NULL)""",
                (message_id, session_id, content, str(origin)[:80], now),
            )
            job_id = uuid.uuid4().hex
            connection.execute(
                "INSERT INTO jobs VALUES (?, ?, ?, 'queued', NULL, '', ?, ?)",
                (job_id, session_id, message_id, now, now),
            )
            connection.execute(
                "UPDATE sessions SET updated_at = ? WHERE id = ?", (now, session_id)
            )
        return {"job_id": job_id, "status": "queued", "duplicate": False}

    def begin_job(self, job_id: str) -> dict[str, Any]:
        now = int(self.clock())
        with self._connect() as connection:
            connection.execute(
                "UPDATE jobs SET status = 'thinking', updated_at = ? WHERE id = ? AND status = 'queued'",
                (now, job_id),
            )
            row = connection.execute(
                """SELECT jobs.*, messages.content AS question, messages.cursor AS question_cursor
                   FROM jobs JOIN messages ON messages.id = jobs.user_message_id
                   WHERE jobs.id = ?""",
                (job_id,),
            ).fetchone()
        if row is None:
            raise KeyError("companion job was not found")
        return dict(row)

    def cancel_job(self, job_id: str) -> dict[str, Any]:
        """Stop a queued/thinking job without deleting its user message."""
        now = int(self.clock())
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                "SELECT status FROM jobs WHERE id = ?", (job_id,)
            ).fetchone()
            if row is None:
                raise KeyError("companion job was not found")
            status = str(row["status"])
            if status in {"queued", "thinking"}:
                connection.execute(
                    """UPDATE jobs SET status = 'cancelled', error = '', updated_at = ?
                       WHERE id = ? AND status IN ('queued', 'thinking')""",
                    (now, job_id),
                )
                status = "cancelled"
            return {
                "job_id": job_id,
                "status": status,
                "cancelled": status == "cancelled",
            }

    def history_before(self, session_id: str, cursor: int) -> list[dict[str, str]]:
        with self._connect() as connection:
            rows = connection.execute(
                """SELECT role, content FROM messages
                   WHERE session_id = ? AND cursor < ? AND role IN ('user', 'assistant')
                   ORDER BY cursor ASC""",
                (session_id, cursor),
            ).fetchall()
        return [{"role": row["role"], "content": row["content"]} for row in rows]

    def finish_job(self, job_id: str, report: dict[str, Any]) -> str | None:
        now = int(self.clock())
        assistant_id = uuid.uuid4().hex
        content = _report_message_content(report)
        if not content:
            raise ValueError("Crew Chief response summary was empty")
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            job = connection.execute(
                "SELECT session_id, status, assistant_message_id FROM jobs WHERE id = ?",
                (job_id,),
            ).fetchone()
            if job is None:
                raise KeyError("companion job was not found")
            if job["status"] == "cancelled":
                return None
            if job["status"] == "completed":
                return str(job["assistant_message_id"] or "")
            if job["status"] != "thinking":
                raise ValueError("companion job is not active")
            connection.execute(
                """INSERT INTO messages
                   (id, session_id, role, content, origin, created_at, report_json)
                   VALUES (?, ?, 'assistant', ?, 'crew-chief', ?, ?)""",
                (assistant_id, job["session_id"], content, now, _json(report)),
            )
            connection.execute(
                """UPDATE jobs SET status = 'completed', assistant_message_id = ?,
                   error = '', updated_at = ? WHERE id = ?""",
                (assistant_id, now, job_id),
            )
            connection.execute(
                "UPDATE sessions SET updated_at = ? WHERE id = ?", (now, job["session_id"])
            )
        return assistant_id

    def fail_job(self, job_id: str, error: str) -> None:
        with self._connect() as connection:
            connection.execute(
                """UPDATE jobs SET status = 'failed', error = ?, updated_at = ?
                   WHERE id = ? AND status IN ('queued', 'thinking')""",
                (str(error)[:1000], int(self.clock()), job_id),
            )

    def get_job(self, job_id: str) -> dict[str, Any]:
        with self._connect() as connection:
            row = connection.execute("SELECT * FROM jobs WHERE id = ?", (job_id,)).fetchone()
        if row is None:
            raise KeyError("companion job was not found")
        return dict(row)

    def backup_and_clear(self, session_id: str) -> dict[str, Any]:
        session = self.get_session(session_id)
        transcript = self.list_messages(
            session_id, 0, limit=MAX_MESSAGES_PER_SESSION
        )
        timestamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime(self.clock()))
        backup_name = f"companion-{session_id}-{timestamp}.json"
        backup_path = self.backup_directory / backup_name
        payload = {
            "contract": COMPANION_CONTRACT,
            "backed_up_at": int(self.clock()),
            "session": session,
            "messages": transcript["messages"],
        }
        temporary = backup_path.with_suffix(backup_path.suffix + ".tmp")
        temporary.write_text(json.dumps(payload, indent=2, ensure_ascii=True), encoding="utf-8")
        temporary.replace(backup_path)
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            connection.execute("DELETE FROM jobs WHERE session_id = ?", (session_id,))
            connection.execute("DELETE FROM messages WHERE session_id = ?", (session_id,))
            connection.execute(
                "UPDATE sessions SET updated_at = ? WHERE id = ?",
                (int(self.clock()), session_id),
            )
        return {
            "backup_created": True,
            "backup_name": backup_name,
            "cleared_messages": transcript["total"],
        }
