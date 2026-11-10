#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# By contributing to this project, you agree to license your contributions
# under the GPLv3 (or any later version) or any future licenses chosen by
# the project author(s).
#
# The gateway's state: tasks, the event log DAWN reads from any sequence
# number, the requests waiting on the operator, and standing grants.  One
# SQLite file; every call runs on the event loop thread, so no locking.

import asyncio
import json
import sqlite3
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS tasks (
   id TEXT PRIMARY KEY,
   dawn_user_id INTEGER NOT NULL,
   agent TEXT NOT NULL,
   repo TEXT NOT NULL,
   base TEXT NOT NULL,
   branch TEXT NOT NULL,
   worktree TEXT NOT NULL,
   task TEXT NOT NULL,
   status TEXT NOT NULL,
   session_id TEXT,
   budget_usd REAL NOT NULL,
   cost_usd REAL NOT NULL DEFAULT 0,
   session_cost REAL NOT NULL DEFAULT 0,
   summary TEXT,
   error TEXT,
   client_ref TEXT,
   published TEXT,
   model TEXT,
   closed REAL,
   created REAL NOT NULL,
   updated REAL NOT NULL,
   UNIQUE (dawn_user_id, client_ref)
);
CREATE TABLE IF NOT EXISTS events (
   seq INTEGER PRIMARY KEY AUTOINCREMENT,
   task_id TEXT NOT NULL,
   ts REAL NOT NULL,
   kind TEXT NOT NULL,
   data TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS requests (
   id INTEGER PRIMARY KEY AUTOINCREMENT,
   task_id TEXT NOT NULL,
   kind TEXT NOT NULL,
   payload TEXT NOT NULL,
   status TEXT NOT NULL,
   answer TEXT,
   note TEXT,
   delivered INTEGER NOT NULL DEFAULT 0,
   created REAL NOT NULL,
   decided REAL
);
CREATE TABLE IF NOT EXISTS grants (
   id INTEGER PRIMARY KEY AUTOINCREMENT,
   dawn_user_id INTEGER NOT NULL,
   kind TEXT NOT NULL,
   value TEXT NOT NULL,
   repo TEXT,
   task_id TEXT,
   created REAL NOT NULL,
   revoked REAL
);
CREATE TABLE IF NOT EXISTS meta (
   key TEXT PRIMARY KEY,
   value TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_events_kind_ts ON events(kind, ts);
"""

# Bumped with each schema change; Store refuses a database it doesn't know.
SCHEMA_VERSION = 3

# Older databases, brought up to this schema in order: version -> its statements.
MIGRATIONS = {
   2: "ALTER TABLE tasks ADD COLUMN model TEXT;",  # the model a task's agent runs on
   3: "ALTER TABLE tasks ADD COLUMN closed REAL;",  # when its work was dropped (done with)
}

# Statuses follow the MCP Tasks vocabulary, so moving to that extension later
# is a rename.
WORKING = "working"
INPUT_REQUIRED = "input_required"
COMPLETED = "completed"
FAILED = "failed"
CANCELLED = "cancelled"
OPEN = (WORKING, INPUT_REQUIRED)


class Store:
   def __init__(self, path: str, check_same_thread: bool = True):
      self.db = sqlite3.connect(path, check_same_thread=check_same_thread)
      self.db.row_factory = sqlite3.Row
      # NORMAL is safe with WAL: a power cut can lose the last commits, never
      # the file.  It keeps an fsync off every event.
      self.db.executescript("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;")
      version = self.db.execute("PRAGMA user_version").fetchone()[0]
      if version == 0:
         self.db.executescript(SCHEMA + f"PRAGMA user_version={SCHEMA_VERSION};")
      elif version > SCHEMA_VERSION:
         raise RuntimeError(f"gateway database is schema {version}; this gateway knows "
                            f"{SCHEMA_VERSION}")
      else:
         for v in range(version + 1, SCHEMA_VERSION + 1):
            self.db.executescript("BEGIN;" + MIGRATIONS[v] + f"PRAGMA user_version={v};COMMIT;")
      self.new_event = asyncio.Event()

   # --- tasks -------------------------------------------------------------

   def add_task(self, **t) -> None:
      now = time.time()
      t.update(status=WORKING, created=now, updated=now)
      cols = ", ".join(t)
      self.db.execute(f"INSERT INTO tasks ({cols}) VALUES ({', '.join('?' * len(t))})",
                      tuple(t.values()))
      self.db.commit()

   def task_by_ref(self, dawn_user_id: int, client_ref: str) -> dict | None:
      row = self.db.execute("SELECT * FROM tasks WHERE dawn_user_id = ? AND client_ref = ?",
                            (dawn_user_id, client_ref)).fetchone()
      return dict(row) if row else None

   def task(self, task_id: str) -> dict | None:
      row = self.db.execute("SELECT * FROM tasks WHERE id = ?", (task_id,)).fetchone()
      return dict(row) if row else None

   def tasks(self, dawn_user_id: int | None = None, open_only: bool = False,
             status: str | None = None, limit: int | None = 50) -> list[dict]:
      sql, args = "SELECT * FROM tasks WHERE 1", []
      if status:
         sql += " AND status = ?"
         args.append(status)
      if dawn_user_id is not None:
         sql += " AND dawn_user_id = ?"
         args.append(dawn_user_id)
      if open_only:
         sql += " AND status IN (?, ?)"
         args += list(OPEN)
      sql += " ORDER BY created DESC"
      if limit is not None:
         sql += f" LIMIT {int(limit)}"
      return [dict(r) for r in self.db.execute(sql, args)]

   def stale_finished(self, before: float) -> list[str]:
      rows = self.db.execute(
         "SELECT id FROM tasks WHERE status IN (?, ?, ?) AND updated < ? AND worktree != ''",
         (COMPLETED, FAILED, CANCELLED, before))
      return [r["id"] for r in rows]

   def update_task(self, task_id: str, **fields) -> None:
      fields["updated"] = time.time()
      sets = ", ".join(f"{k} = ?" for k in fields)
      self.db.execute(f"UPDATE tasks SET {sets} WHERE id = ?", (*fields.values(), task_id))
      self.db.commit()

   def set_published(self, task_id: str, record: str) -> None:
      """Bookkeeping on the publish record; `updated` left alone."""
      self.db.execute("UPDATE tasks SET published = ? WHERE id = ?", (record, task_id))
      self.db.commit()

   def set_closed(self, task_id: str) -> None:
      """When the task was closed; `updated` left alone."""
      self.db.execute("UPDATE tasks SET closed = ? WHERE id = ?", (time.time(), task_id))
      self.db.commit()

   def add_cost(self, task_id: str, usd: float) -> None:
      self.db.execute("UPDATE tasks SET cost_usd = cost_usd + ? WHERE id = ?", (usd, task_id))
      self.db.commit()

   def spent_since(self, since: float) -> float:
      """Spend of every run that ended since `since` (each run logs its cost)."""
      row = self.db.execute(
         "SELECT COALESCE(SUM(json_extract(data, '$.cost_usd')), 0) FROM events "
         "WHERE kind = 'run_end' AND ts >= ?", (since,)).fetchone()
      return row[0]

   # --- events ------------------------------------------------------------

   def add_event(self, task_id: str, kind: str, **data) -> int:
      cur = self.db.execute("INSERT INTO events (task_id, ts, kind, data) VALUES (?, ?, ?, ?)",
                            (task_id, time.time(), kind, json.dumps(data)))
      self.db.commit()
      self.new_event.set()
      return cur.lastrowid

   def task_events(self, task_id: str, kind: str, limit: int) -> list[dict]:
      """A task's last @limit events of @kind, oldest first, data decoded."""
      rows = self.db.execute(
         "SELECT seq, ts, data FROM events WHERE task_id = ? AND kind = ? ORDER BY seq DESC "
         "LIMIT ?", (task_id, kind, limit)).fetchall()
      return [{"seq": r["seq"], "ts": r["ts"], "data": json.loads(r["data"])}
              for r in reversed(rows)]

   def events_after(self, after_seq: int, limit: int = 200,
                    max_bytes: int = 256 * 1024) -> list[dict]:
      """Events in order, each with its task's user and client_ref (system events,
      task_id '', have neither).  A batch stops at `max_bytes` of event data (at
      least one event), so a reply stays well within a client's size limit."""
      rows = self.db.execute(
         "SELECT e.seq, e.task_id, e.ts, e.kind, e.data, t.dawn_user_id, t.client_ref "
         "FROM events e LEFT JOIN tasks t ON t.id = e.task_id WHERE e.seq > ? "
         "ORDER BY e.seq LIMIT ?", (after_seq, limit))
      out, size = [], 0
      for r in rows:
         size += len(r["data"])
         if out and size > max_bytes:
            break
         out.append({**dict(r), "data": json.loads(r["data"])})
      return out

   def latest_seq(self) -> int:
      return self.db.execute("SELECT COALESCE(MAX(seq), 0) FROM events").fetchone()[0]

   async def wait_events(self, after_seq: int, wait_s: float) -> list[dict]:
      deadline = asyncio.get_running_loop().time() + wait_s
      while True:
         self.new_event.clear()
         found = self.events_after(after_seq)
         left = deadline - asyncio.get_running_loop().time()
         if found or left <= 0:
            return found
         try:
            await asyncio.wait_for(self.new_event.wait(), left)
         except TimeoutError:
            pass

   def meta(self, key: str) -> str | None:
      row = self.db.execute("SELECT value FROM meta WHERE key = ?", (key,)).fetchone()
      return row["value"] if row else None

   def set_meta(self, key: str, value: str) -> None:
      self.db.execute("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)", (key, value))
      self.db.commit()

   # --- requests ----------------------------------------------------------

   def add_request(self, task_id: str, kind: str, payload: dict) -> int:
      cur = self.db.execute(
         "INSERT INTO requests (task_id, kind, payload, status, created) VALUES (?, ?, ?, ?, ?)",
         (task_id, kind, json.dumps(payload), "pending", time.time()))
      self.db.commit()
      return cur.lastrowid

   def request(self, request_id: int) -> dict | None:
      row = self.db.execute("SELECT * FROM requests WHERE id = ?", (request_id,)).fetchone()
      return {**dict(row), "payload": json.loads(row["payload"])} if row else None

   def pending_requests(self, task_id: str) -> list[dict]:
      rows = self.db.execute("SELECT id FROM requests WHERE task_id = ? AND status = 'pending'",
                             (task_id,))
      return [self.request(r["id"]) for r in rows]

   def decide_request(self, request_id: int, status: str, answer: str | None = None,
                      note: str | None = None) -> None:
      """Record the operator's decision; `note` is what the agent is told on resume."""
      self.db.execute(
         "UPDATE requests SET status = ?, answer = ?, note = ?, decided = ? WHERE id = ?",
         (status, answer, note, time.time(), request_id))
      self.db.commit()

   def add_note(self, task_id: str, note: str) -> None:
      """Something to tell the agent on its next resume (not a request)."""
      self.db.execute(
         "INSERT INTO requests (task_id, kind, payload, status, note, created) "
         "VALUES (?, 'notice', '{}', 'decided', ?, ?)", (task_id, note, time.time()))
      self.db.commit()

   def has_notes(self, task_id: str) -> bool:
      return self.db.execute(
         "SELECT 1 FROM requests WHERE task_id = ? AND note IS NOT NULL AND delivered = 0",
         (task_id,)).fetchone() is not None

   def take_notes(self, task_id: str) -> list[str]:
      """Decisions not yet told to the agent, marked as told."""
      rows = self.db.execute(
         "SELECT id, note FROM requests WHERE task_id = ? AND note IS NOT NULL AND delivered = 0 "
         "ORDER BY id", (task_id,)).fetchall()
      self.db.executemany("UPDATE requests SET delivered = 1 WHERE id = ?",
                          [(r["id"],) for r in rows])
      self.db.commit()
      return [r["note"] for r in rows]

   # --- grants ------------------------------------------------------------

   def add_grant(self, dawn_user_id: int, kind: str, value: str, repo: str | None,
                 task_id: str | None) -> int:
      cur = self.db.execute(
         "INSERT INTO grants (dawn_user_id, kind, value, repo, task_id, created) "
         "VALUES (?, ?, ?, ?, ?, ?)", (dawn_user_id, kind, value, repo, task_id, time.time()))
      self.db.commit()
      return cur.lastrowid

   def grants(self, dawn_user_id: int, include_task_scoped: bool = False) -> list[dict]:
      sql = "SELECT * FROM grants WHERE dawn_user_id = ? AND revoked IS NULL"
      if not include_task_scoped:
         sql += " AND task_id IS NULL"
      return [dict(r) for r in self.db.execute(sql + " ORDER BY id", (dawn_user_id,))]

   def grants_for(self, task: dict, kind: str) -> list[str]:
      """Values granted to this task: its own, its repo's, and every repo's."""
      rows = self.db.execute(
         "SELECT value FROM grants WHERE dawn_user_id = ? AND kind = ? AND revoked IS NULL "
         "AND (task_id = ? OR (task_id IS NULL AND (repo IS NULL OR repo = ?)))",
         (task["dawn_user_id"], kind, task["id"], task["repo"]))
      return sorted({r["value"] for r in rows})

   def revoke_grant(self, grant_id: int, dawn_user_id: int) -> bool:
      cur = self.db.execute(
         "UPDATE grants SET revoked = ? WHERE id = ? AND dawn_user_id = ? AND revoked IS NULL",
         (time.time(), grant_id, dawn_user_id))
      self.db.commit()
      return cur.rowcount == 1
