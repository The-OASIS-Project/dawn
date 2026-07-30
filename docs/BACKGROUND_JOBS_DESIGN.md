# Durable Background Jobs — Design

**Status:** LIVING DESIGN — the north-star plan for the durable-background-jobs program. Phase 0, Phase 1,
and the server half of Phase 2 are shipped; Phases 2 (browser surface), 3, 4, and 5 are ahead. This doc is
the **plan, scope, and design details** — not a build log. The shipped-commit history lives in git and the
commit messages; when the program completes, the design record graduates to `atlas/dawn/archive/`.

- **Current state (one line):** background turns generate independently of the viewing socket (P1); text-only
  jobs run in a separate pool with a lifecycle, tool, and completion delivery (P2-minimal); a durable event
  log + attach/replay + a jobs panel expose them (Phase 2 server half). Trees, the browser transcript
  surface, workspace/sandbox, and the TUI are ahead.
- **Where the details live:** §3 data model · §4/§4a hazards + the turn-queue foundation · §5 lifecycle ·
  §6 stream/observe contract · §7 config · §8 security invariants · §10 phased plan with status · §14
  shipped/remaining ledger + known open defects.
- Authored 2026-07-22. This revision reconciles the doc to the code as of the `background-jobs-p2-observe`
  branch (schema v74).

> **Reviewer note:** `docs/TODO.md` / `docs/DONE.md` are developer-local (gitignored); references to them
> won't resolve in a checkout. §14 is self-contained for shipped/remaining state; the known-open-defect list
> at the end of §14 is the authoritative "what's broken now" record.

---

## 1. Goal & Framing

"Durable background jobs" and "a conversation keeps generating while I look at another one" are **two products on one primitive**:

> The generation loop is owned by the daemon/session, not by the websocket currently viewing it, and its output goes to a durable per-conversation stream that any client can attach to.

Products on that primitive, in increasing size:

- **P1** — a turn keeps generating while you switch conversations; output lands in the right conversation, not the active view.
- **P2** — fire-and-forget background job ("research X" / "compile+test Y, tell me when done"), delivered by notification or agentic follow-up.
- **P3** — many conversations autonomously working in parallel (the Codex-Cloud / Cursor "task list" model, applied to conversations).

**Core design decision:** a job **is** a parented conversation. We reuse the entire conversation stack (persistence, history, tool loop, reconnect-replay) and add a parent edge + a small lifecycle — **no parallel job subsystem.**

### Forward constraint (designed-in from Phase 0)

A terminal/VM sandbox where agents create files, compile, and test is coming. DAWN's hard rule (CI-enforced): **DAWN never spawns/forks/execs external processes.** The operator launches the sandbox; DAWN **connects over MCP** (the coding-harness Phase 1 pattern). A job's *body* is therefore a parented conversation whose tool-loop makes MCP calls into the operator-launched sandbox — the execution model is unchanged by adding the VM. The only additions are a durable `workspace_ref` (so a restart reattaches instead of orphaning), a workspace-inheritance policy on spawn, and terminal output as another event kind in the same durable stream.

---

## 2. What we reuse vs. what is new

**Reuse — DAWN already owns these:**

- Conversations are **session-independent**, durably persisted, created/loaded/searched/deleted by `(conv_id, user_id)` — `conv_db_*` in `src/auth/auth_db_conv.c`. A conversation exists with no live session.
- A **self-FK precedent** exists: `continued_from INTEGER → conversations(id) ON DELETE SET NULL`. `parent_id` is a sibling edge (distinct — `continued_from` = compaction lineage).
- **Generation is bound to the session, not the fd**: the chunk callback captures a session pointer; the socket is resolved at emit time from `session->client_data`, so reconnect re-homes a live stream.
- **No global gate on the WebUI path**: `llm_processing`/single `llm_thread` is voice/Session-0 only. WebUI/DAP2/messaging already spawn one detached worker per request keyed to a session.
- The **ownership-JOIN authorization pattern**: `conv_db_get_messages` binds `user_id` via a JOIN; every WS conv handler binds `conn->auth_user_id`. New surfaces reuse this exactly.
- Completion **delivery** (chime/banner/voice/`deliver_to` channel) is built in scheduler/briefing + messaging; SAGE attention has notification budgets/quiet-hours to coalesce against.
- The main-loop **1-second heartbeat** drives periodic work with no dedicated thread (OTA rollout precedent).

**New:**

- v72–v74 schema columns + a `conversation_events` step-log table.
- `conversation_id`-tagged deltas + a conversation-keyed in-memory replay ring (`src/core/conv_stream.c`, Layer-1 leaf).
- A **separate job-session pool** (own storage + lifecycle) — not the interactive `sessions[8]` array (§4).
- A **per-session turn queue** (`src/core/turn_queue.c`, Layer-1 leaf) serializing all turn producers on a session (§4a).
- `src/tools/job_tool.c` (LLM tool) + a WebUI jobs surface (`www/js/ui/jobs.js`).
- A completion monitor tick on the existing heartbeat.
- `reinvoke_parent` DB-mediated follow-up with budgets.
- The durable event log + attach/replay contract (`conv_event.c`, `event_payload.c`, `auth_db_events.c`).

---

## 3. Data Model

A job is a `conversations` row with `parent_id` set (or, for a root user-initiated job, `job_status` set with `parent_id` NULL). Job columns are all **nullable / literal-defaulted** → fast ALTER, no table rewrite.

**13 job columns** (v72 added 11, v73 added `deliver_to`, v74 added `job_goal`):

| Column | Type | Meaning |
|---|---|---|
| `parent_id` | INTEGER, FK → conversations(id) ON DELETE SET NULL | Who spawned me. NULL = root/user-initiated. Distinct from `continued_from`. |
| `spawn_mode` | TEXT | `detached` only in v1. (`awaited` is UI sugar over detached+reinvoke — §5.) |
| `on_complete` | TEXT | `notify` \| `reinvoke_parent` \| `none`. |
| `on_complete_fired` | INTEGER DEFAULT 0 | Follow-up idempotency flag (Odysseus `followed_up`). Set only after notify/reinvoke succeeds; cancel sets it to suppress follow-up. **Overloaded — see §14 known defects; splitting it is a Phase-3 prerequisite.** |
| `job_status` | TEXT DEFAULT NULL | NULL = **not a job**. Else `queued` \| `running` \| `done` \| `failed` \| `interrupted` \| `cancelled`. Orthogonal to `is_archived`/`is_private`. |
| `job_error` | TEXT DEFAULT NULL | Failure reason (sanitized — §8). |
| `spawn_depth` | INTEGER DEFAULT 0 | 0 = root. **v1: hardcoded to 1 on create;** `= parent+1` propagation + depth/children caps are Phase 3. |
| `reinvoke_count` | INTEGER DEFAULT 0 | Reinvoke re-dispatch counter (livelock guard, §5). |
| `started_at` | INTEGER DEFAULT 0 | Job run start (epoch). |
| `finished_at` | INTEGER DEFAULT 0 | Job terminal time (epoch). Also the restart-vs-this-run discriminator (§5 restart). |
| `workspace_ref` | TEXT DEFAULT NULL | Opaque sandbox handle (MCP server id + workspace id + generation nonce). Semantics land in Phase 4; reserved now. |
| `deliver_to` | TEXT DEFAULT NULL | Completion-notification target for `notify` (messaging-origin job answers on its own channel; same semantics as scheduler `deliver_to`). Bound by `conv_db_create_job`; read by the monitor's notify batch. |
| `job_goal` | TEXT DEFAULT NULL | The instruction the job was created with, durable **from CREATE** so a capacity-refused job can still be resumed (§5 resume). Read only by the resume path; deliberately not on `job_record_t` (that struct is bulk-copied for snapshots). |

`user_id` is **not** a job column — the child inherits the spawner's `user_id` via `conv_db_create` and every read is ownership-JOINed (§8). A tree is single-user by invariant. **`is_private` is likewise inherited** from the parent (existing column, not new), so a job under a private conversation is private.

**Fan-out/join is derived, not stored** — "parent resumable iff it has no `running` children with `on_complete='reinvoke_parent'`" is one index-assisted scan over the (≤4) child set. Index `idx_conversations_parent (parent_id, job_status)`. A `conversation_waits` table is added only if multi-child *ordered* joins ever materialize (deferred).

### `conversation_events` (durable step log)

Step-granular, **never token-granular** (no leader persists tokens; a per-delta SQLite write is untenable on Jetson).

```sql
CREATE TABLE IF NOT EXISTS conversation_events (
   id INTEGER PRIMARY KEY AUTOINCREMENT,
   conversation_id INTEGER NOT NULL,
   seq INTEGER NOT NULL,              -- per-conversation monotonic
   kind TEXT NOT NULL,               -- status | tool_call | tool_result | terminal_chunk | spawn | complete
   payload TEXT,                     -- kind-specific JSON, secret-redacted + size-capped (event_chunk_cap)
   created_at INTEGER NOT NULL,
   FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_conv_events ON conversation_events(conversation_id, seq ASC);
```

- **No `user_id` column** (mirrors `messages`); every read JOINs `conversations` and filters `user_id` (§8.3).
- `seq` = `MAX(seq)+1` under the auth_db write lock, with a UNIQUE backstop. If step-granular contention shows, switch to an in-memory per-conversation counter in `conv_stream` (deferred).
- Final assistant text is **not** duplicated here — it lives in `messages`. `complete` carries `final_message_id`, and the finished text is pushed as a `message_appended` frame so a pure tailer gets the answer (§6).
- Retention is kind-aware (§8.8), driven by `event_retention_days` (default 30, ON).

**Migration discipline:** each version's body is its own `auth_db_migrations_v7N.c` (matching `_v67.c`); only the ~15-line AND-gated ladder block is appended to the size-exempt `auth_db_migrations.c`. `conv_column_exists()` probe + literal-DEFAULT. **Current schema version: 74** (`AUTH_DB_SCHEMA_VERSION`, `include/auth/auth_db_internal.h`).

---

## 4. The three hazards and their resolution

1. **Output dropped when unattached.** Chunk callbacks early-return on `session->disconnected` and queued responses are freed undelivered. **Fix (the core inversion):** deltas publish to a conversation-keyed in-memory replay ring (`conv_stream.c`) tagged with `conversation_id`; step transitions persist to `conversation_events`. A client attaching (or on another conversation) replays from the ring + durable events. Live token emission still gates on an attached client, but content is no longer lost — replayable (ring) and durable (events).

2. **8-session ceiling is audio-bound and does not apply to sub-agents.** `MAX_SESSIONS 8` exists because each interactive session is **audio-pipeline-bound** (Whisper/Vosk recognizer, TTS access, an audio worker); all current session types allocate from one global `sessions[8]` array via `find_free_slot()`, and messaging's `MESSAGING_MAX_SESSIONS=64` is an LRU *directory* over those 8 slots, not a separate pool. **Fix:** a background job is text-only (LLM tool-loop + MCP; no ASR/TTS/audio), so it gets a **separate job-session pool** — its own `session_t` storage, `find_free_job_slot`, `session_get_job_by_id`, job-pool teardown — **not** counted against `MAX_SESSIONS`. The `session_t` *type* and every `session_t*`-taking worker/streaming path are unchanged. The pool cap is **LLM-resource-bound** (the `max_concurrent_local`/`_cloud` counters + security caps), never audio. Consequence: a running job can never starve an interactive browser/satellite, and interactive `session_destroy` is untouched by job lifecycle.

3. **Ref-count 3s-timeout UAF.** Interactive `session_destroy` waits 3s for `ref_count==0` then proceeds anyway. Jobs live in a separate pool, so their teardown is a **new** path: a worker holds a ref for its whole run; job-pool teardown is **cancel-then-wait** (set `cancel_requested`, worker observes and unrefs, then free) — no "proceed anyway." The interactive path's teardown safety is addressed by §4a. `session_get_local()`'s no-ref return stays a documented voice-only exception.

**No new cross-thread history writers.** Job workers write only their own session's history (existing `history_mutex`). Parent follow-up is **DB-mediated and dispatched only when the parent is idle** (§5) — never a cross-thread `session_add_message` into a live session. This composes with the `conversation_history` serialization fix (§9); state it in-code so a future implementer doesn't take the live-inject shortcut.

**Cancel / `disconnected` decoupling (for P1 disconnect-survival):** live-turn cancellation uses a per-session `cancel_requested` atomic; `disconnected` then gates emission only, so an interactive backgrounded turn survives a tab close. This touches **all five** former `llm_set_cancel_flag(&session->disconnected)` sites — 3× streaming (`session_manager_llm.c`) **and** async compaction (`llm_context.c`), because `session_destroy` joins the compaction thread relying on `disconnected` aborting its CURL. Invariant: `session_destroy` sets **both** `disconnected` and `cancel_requested`. A destroy-during-compaction ThreadSanitizer test guards it.

---

## 4a. Per-session turn queue — the reinvoke concurrency foundation

Re-engaging a parent by streaming into the user's **live** viewer session races the user's own turn on that same `session_t` (two turns stomping one session's per-turn streaming state); `request_generation` is a post-hoc "who persists" arbiter, not a lock. The resolution is infrastructure the feature rests on:

- **`src/core/turn_queue.c` (Layer-1 leaf).** A per-session FIFO guaranteeing **at most one turn runs on a given `session_t` at a time**. All WebUI turn producers — user **text**, **push-to-talk voice**, and **background reinvoke** — enqueue instead of `pthread_create`-ing the worker directly. "Two turns never touch one session's streaming state concurrently" becomes a structural property. One leaf mutex, never held across the spawn/free closures; `TURN_SOURCE_USER` (cap 16) vs `TURN_SOURCE_BACKGROUND` (cap 8).
- **Reinvoke rides the queue.** `reinvoke_run` finds+retains the live viewer and enqueues a marker as a background turn (or falls back to a detached job-pool session when there's no viewer). The envelope is built at **dequeue** (re-querying the parent's pending jobs so late siblings coalesce), with an **exactly-once release matrix** for `{session_release, inflight_release, mark_dirty, free}` across every path. A re-engagement is marked fired only once its reply is actually saved (client-saved foreground, or server-persisted when backgrounded/client-gone).
- **Teardown safety (closes Hazard 3 for the interactive path).** `session_t.being_destroyed` (set first in `session_destroy`) + a slot `closing` flag + purge-before-ref-wait; `session_begin_turn_flags` **no-ops under being_destroyed** so a turn spawned into the pop-to-run window can't resurrect the teardown cancel. The interactive ref-wait is **bounded-retry-then-leak-don't-free** (`SESSION_DESTROY_REF_WAIT_MAX_SEC` = 30) — leaking a few-KB `session_t` is recoverable; freeing it under a running worker is a UAF. `session_t.last_activity` is `_Atomic` (was written under `history_mutex`, read under `session_manager_rwlock`).

**Producer coverage:** the DAP2 `satellite_query` path is the one turn producer **not** yet on the queue (distinct session objects + reinvoke only targets WEBUI sessions, so the invariant holds today). Route it through when a third concurrent producer per satellite session appears.

---

## 5. Lifecycle

> **Current-build divergences from the design intent below** (tracked in §14): (1) `reinvoke_parent` re-engages via the turn queue (§4a), not a direct DB-inject-and-redispatch; (2) there is **no queued *state machine*** — a row is inserted `queued` and cancel handles that status, but the worker promotes it straight to `running`; nothing parks in `queued`, there's no monitor promotion, and over-cap spawns **fail fast** (so `max_queued_per_user` is unenforced); (3) `spawn_depth` is hardcoded to 1 (real depth + caps are Phase 3).

### Spawn

Jobs are created via the LLM tool (`job_tool.c`, actions `spawn`/`list`/`status`/`cancel`/`resume`) and the WebUI — never TOML (JARVIS conversational-control principle). Spawn:

1. Creates a child conversation (`conv_db_create`) with the **spawner's `user_id` (non-overridable)**, `parent_id`, `spawn_mode`, `on_complete`, `spawn_depth` (Phase 3: `parent+1`), `job_status='queued'`, `job_goal`, inheriting the parent's per-conversation LLM settings by default.
2. **Budget gates — refuse (not silently enqueue) past a cap:** global active `≤ max_active_jobs`; per-user concurrent `≤ max_jobs_per_user`; per-provider running counter (`max_concurrent_local`/`_cloud`). *(Phase 3 adds `spawn_depth ≤ max_spawn_depth`, tree children `≤ max_children_per_tree`, `≤ max_queued_per_user`, and the spawn-into-cancelling-tree guard.)*
3. When a slot + provider counter are available → `job_status='running'`, `started_at=now`, emit `status` + `spawn` events.

### `awaited` presented, `detached` executed

`awaited` is **not** a blocking code path (a parked parent wastes a slot and risks starvation). The parent's turn **ends**; `on_complete='reinvoke_parent'` makes the child's completion re-dispatch the parent. UI may *present* "waiting for subtask"; no slot blocks.

### Completion + follow-up (monitor on the 1s heartbeat — no new thread)

The monitor is **dirty-gated** (zero DB work when idle) and drains up to `monitor_followups_per_tick` rows where `job_status IN (done,failed,interrupted) AND on_complete_fired=0`:

- `on_complete='notify'` → deliver via `job_notify_user()` (toast the owner's browsers, else queue a missed notification — durable, so the row retires immediately); `deliver_to` messaging path keeps a bounded retry. Flip `on_complete_fired=1` only on delivery.
- `on_complete='reinvoke_parent'` → **defer if the parent has a turn in flight**; when idle, enqueue a background turn on the parent (§4a), `reinvoke_count++`. Deferral is bounded (`JOB_REINVOKE_MAX_DEFER_SEC`); past it, degrade to a notification. If `reinvoke_count ≥ max_reinvokes_per_tree`, stop re-dispatching and notify (livelock guard). **Every row routed to the reinvoke processor must leave the follow-up set** — an orphaned `reinvoke_parent` row (parent deleted, `ON DELETE SET NULL`) fires as a notification rather than stranding the global, `finished_at ASC`-ordered scan for all users.

### Runtime reap (`max_runtime_sec`)

`job_manager_reap_overdue(now)` runs from the monitor tick **ahead of the dirty gate** (a wedged job never marks anything dirty), walking the **in-memory pool only** (each slot carries `started_at`; no DB read on the voice loop) and early-outing when nothing runs. A reap is a cancel **request**, escalating: (1) flag + `session_cancel_turn()`; (2) nag every `JOB_REAP_NAG_INTERVAL_SEC`; (3) after `JOB_REAP_FORCE_RELEASE_SEC`, reclaim the **provider counter** so a job wedged inside a tool call (where the session cancel flag isn't polled — see §14 known defects) can't hold `max_concurrent_local` forever. The worker snapshots `cancel_requested` **before** claiming the reap verdict, and **persists any produced answer regardless of disposition**. Precedence: user-cancel (silent, keep answer) → answer produced (`done`) → reaped with nothing (`failed`+`JOB_ERR_TIMED_OUT`, follow-up still owed) → empty. `max_runtime_sec ≤ 0` disables; a backward clock reaps nothing.

**Pool sizing:** the slot array is allocated once to a fixed `JOB_POOL_MAX_SLOTS` (256), **not** to `max_active_jobs` (which is runtime-mutable via settings). `max_active_jobs` is a pure policy counter; the constant must stay ≥ the `config_clamp_jobs()` ceiling, else a raised cap could pass `job_manager_capacity()` while `job_manager_begin_ex()` finds no slot.

### Cancel (ownership-bounded)

Cancel sets the target's `cancel_requested`, marks `job_status='cancelled'`, sets `on_complete_fired=1` (suppress follow-up). A running job is signalled through its session; a queued one has no session and is retired directly — both live in `job_manager_cancel_or_retire()` so the tool and WS surface share one mutation path. Best-effort MCP cancel is sent to the sandbox; DAWN cancels its *conversation*, not the operator-owned remote process.

**Parent-delete cascade (single level, shipped).** Deleting a parent conversation would otherwise leave its job children orphaned — `parent_id` is `ON DELETE SET NULL`. So the delete handler (`webui_history.c`) first cancels each direct job child (`job_manager_cancel_or_retire`) and deletes it (`conv_db_job_list_children` → `conv_db_delete`, freeing the child's images the same way the parent's are), ownership-scoped. Single level is complete today because job→job spawning is blocked (`spawn_depth ≡ 1`). *(Phase 3 generalizes this to multi-level trees with the `WHERE user_id = caller` descendant walk + the reap-before-FK-null ordering; the single-level path is a subset of that work.)*

### Restart semantics (honest, structural)

The job body dies with the daemon (no-subprocess rule). On boot: scan `job_status='running'` → set `interrupted` + `job_error`, stamp `finished_at = s_boot_time - 1` (the work stopped when the *previous* daemon died), queue a notification. The monitor's restart-safety rule is per-tick on `finished_at < s_boot_time`, so it never auto-reinvokes a parent across a restart. If `workspace_ref` is present (Phase 4), attempt MCP reattach only after validating its generation nonce. **Resume is explicit** (§6) — no seamless survival is promised.

### Memory extraction

Child/job conversations (`job_status IS NOT NULL`) are **exempt** from session-end memory extraction (the notes-store `note_extraction_guard` lesson — avoids poisoning the fact store with tool noise). The guard lives in the shared `should_skip_memory_extraction()` where all WebUI triggers funnel (job convs became loadable via the panel's View button, so the exemption can't live only in the catch-up scan). Job *results* still reach memory via the **parent** through `reinvoke_parent`, whose normal extraction captures what matters.

---

## 6. Stream & observe contract (WebUI **and** future TUI)

One attach protocol, two renderers. All server→client frames go through the `send_json_response`/`queue_response` funnel (CI-enforced).

**Server → client:**

1. **`delta`** — existing streaming frames + a `conversation_id` field. Ephemeral live tokens; never persisted; replayable only from the in-memory ring.
2. **`conversation_event`** `{conversation_id, seq, kind, payload, created_at}` — durable steps. `kind` ∈ `status`, `tool_call`, `tool_result`, `terminal_chunk` (Phase 4), `spawn` (child conv_id + title), `complete`. The stream a line-printer TUI tails.
   - **`status` is SCOPED**: emitted for job conversations, background/reinvoke turns, and turns whose conversation is a job (`job_status IS NOT NULL`) — three terms because none alone covers a user typing into a job conversation from their own session. Interactive conversations are already observable via the sidebar chip; universal emission roughly doubled the event table for no added visibility.
   - **`tool_call`/`tool_result` are NOT scoped** — they fire for every conversation (real content, and this lets the event log eventually back the debug transcript). *This decision has a cost the current build has not paid down — see §14 known defects (redaction gap + write amplification).*
   - **`complete` carries terminal disposition**: `{disposition, error?, final_message_id}`.
3. **`message_appended`** `{conversation_id, message_id, role, text}` — pushed when a turn's final assistant message persists, so a pure event-tailer receives the **answer body** (which lives in `messages`, not events). Required for the TUI verification.
4. **Job list frames, split by object LIFETIME:**

   | Frame | Carries | Bound |
   |---|---|---|
   | `jobs_snapshot` | the complete ACTIVE set, full rows, on `jobs_request` | structurally bounded (active set ≤ `max_active_jobs`, clamped ≤ 256); says `truncated: true` if ever hit |
   | `job_update` | one job's row, on every lifecycle transition | — |
   | `list_jobs_response` | one keyset-paginated page of TERMINAL jobs (history) | unbounded feed — **never feed a count from it** |
   | `jobs_invalidate` | contentless "re-sync your job views" nudge for STRUCTURAL changes the deltas don't cover (a job removed by the parent-delete cascade) | — |

   `job_update` is a delta (add/change); a removal has no delta because a removed job may live in the paginated history, not the active set — so a `job_removed` delta would have to be applied to both lists. `jobs_invalidate` instead tells the client to re-pull the authoritative snapshot (+ reload history if the panel is open), which is what it already does on reconnect. Emitted browser+owner-only from the delete handler after the cascade.

   Job object: `{conversation_id, parent_id, title, status, spawn_mode, on_complete, spawn_depth, reinvoke_count, created_at, started_at, finished_at, deliver_to?, error?}`.

   Pill counts are derived client-side by grouping the active set on `parent_id` — sound only because the active set is a bounded, complete SET (counts are recoverable from rows, not the reverse). This replaced the Phase-1 count frames (`job_activity`/`jobs_activity_snapshot`).

**Emit ordering — three sites, each owned by the thread that owns the row.** `job_update_emit()` fires at spawn (`job_tool.c`), at `queued→running` (`job_worker.c`), and at every terminal disposition (`job_manager_set_terminal()`). Two invariants:
- The spawn emit runs **before** `job_worker_spawn()` — else the spawning thread can read a stale `queued` row and enqueue it behind the detached worker's terminal frame, pinning a phantom job until reconnect.
- **Terminal is a sink, enforced client-side.** A job finishing between a snapshot's two-step read/enqueue would be resurrected by the snapshot; the client keeps a bounded FIFO of recently-terminal conversation ids and refuses to re-activate them. The sink applies to `jobs_snapshot` **only** (live `job_update`s are ordered at the source); it clears on every `jobs_request`.

**Client → server:**

- **`attach_conversation {conv_id, last_seq}`** (extends `load_conversation`): binds `conn->auth_user_id`, loads ownership-checked, replies with (a) messages, (b) durable events where `seq > last_seq` JOINed on `user_id`, (c) ring replay of the partial turn, then (d) live-tails. Register-subscriber-before-replay + seq-dedup + end-sentinel tail-flush.
- **`jobs_request {}`** → `jobs_snapshot`. **`list_jobs {before_created_at, before_id, limit}`** → `list_jobs_response`.
- **`job_action {action, conversation_id}`**, action ∈ `cancel` | `resume` → `job_action_response`. Every action binds `conn->auth_user_id` and loads the row via `conv_db_job_get` **before acting**; a foreign job and a missing one answer identically (no id oracle). `spawn` is deliberately **not** a WS action — creation is conversational (§7).

**Cancel/resume semantics.** `cancel` retires a running job through its session or a queued row directly (`job_manager_cancel_or_retire()`). `resume` re-dispatches an `interrupted` or `failed` job (`done` is excluded — it has an answer); `cancelled` is admitted **only from the WebUI** (`JOB_RESUME_ORIGIN_USER`), never from the `job` tool (`JOB_RESUME_ORIGIN_TOOL`, reachable by the model during untrusted reinvoke turns): restarting interrupted/failed *restores* the user's intent, restarting cancelled *reverses* it, and the model must not reverse a stop a person asked for. The origin is passed **into** the atomic claim (`conv_db_job_reset_for_resume`), not pre-checked, so the widened predicate stays inside one UPDATE. Resume clears `on_complete_fired` (re-arms delivery), counts as a spawn against caps, hydrates prior messages and dispatches a continuation directive (or re-sends `job_goal` if the job never ran), and carries `resumed: true` (the one transition moving a job *backwards* out of terminal, which clients otherwise treat as a sink). Both surfaces call one `job_worker_resume()`. The headless backstop covers `resume` (it takes a slot like a spawn).

**Ordering unifier:** `messages.id` and `conversation_events.seq` interleave by insertion order; live deltas belong to the not-yet-persisted tail. Renderer (both surfaces): merged messages + events → partial tail from ring replay → live-tail → final text on `message_appended`.

**Layer contract for `conv_stream.c` / `conv_event.c`:** passive **Layer-1 leaf** stores keyed by `conversation_id` (leaf mutex, copy-under-lock, no I/O or send-funnel call while held). Higher layers **pull** on attach; live fan-out to a second attached client is driven from the emit path (Layer 4) / registered-fn bridge — never a Layer-1→Layer-4 upcall. `conv_event_emit()` pairs persist + fan-out in one call (durable-first) so an emit site can't persist-without-broadcasting.

**Untrusted-content discipline (both renderers):** `tool_result` and `terminal_chunk` payloads are attacker-controllable (web fetches, compiled/run output). Persisted secret-redacted (§8.6) and rendered **as text only** — WebUI via `textContent`/DOMPurify + control-sequence stripping; TUI strips ANSI/control bytes. `conversation_events.payload` is untrusted at every read site.

**Surface scope:** job list frames are browser-only (`is_satellite` excluded); event frames are not. The Phase-5 TUI authenticates as an ordinary WS client (`is_satellite` false), so it still receives job frames; `tests/tools/tail_conversation.py` is the reference consumer.

---

## 7. Configuration (`dawn.toml`)

```toml
[jobs]
enabled = true
max_concurrent_local = 1        # single GPU: keep to 1 with a local LLM (LLM-resource-bound, NOT audio-bound)
max_concurrent_cloud = 4        # cloud provider: parallelism is cheap
max_active_jobs = 16            # GLOBAL running ceiling across all users (slot + cost DoS guard)
max_jobs_per_user = 4           # per-user concurrent running
max_queued_per_user = 8         # per-user bounded backlog (Phase 2 queued state — NOT yet enforced)
monitor_followups_per_tick = 4  # bound heartbeat work so the voice loop isn't degraded
max_spawn_depth = 3             # tree depth cap (Phase 3 — NOT yet enforced)
max_children_per_tree = 4       # active children per parent (Phase 3 — NOT yet enforced)
max_reinvokes_per_tree = 8      # reinvoke/timeout-refire livelock guard
max_concurrent_reinvokes = 2    # global cap on in-flight reinvoke_parent workers
max_runtime_sec = 1800          # per-job reap (still fires follow-up; 0 disables)
event_chunk_cap = 16384         # head+tail truncation for tool_result / terminal_chunk payloads
event_retention_days = 30       # age out conversation_events payloads (0 = never)
```

**Enforcement status (as-built):**
- **Enforced:** `enabled`, `max_concurrent_local`/`_cloud`, `max_active_jobs`, `max_jobs_per_user`, `monitor_followups_per_tick`, `max_reinvokes_per_tree`, `max_concurrent_reinvokes`, `max_runtime_sec`, `event_chunk_cap` (truncation cap), `event_retention_days` (retention).
- **Parsed + round-tripped but NOT yet enforced** (Phase 2/3 forward-decls; setting them changes nothing today): `max_queued_per_user` (queued state), `max_spawn_depth`, `max_children_per_tree` (trees). These are deliberately **kept out of the WebUI panel** (a control that does nothing is worse than no control) but still round-trip so a hand-edited value survives a save.

**Two independent provider counters:** a job counts against `max_concurrent_local` or `max_concurrent_cloud` by its resolved provider class (a per-conversation setting can make a child cloud under a local default). Caps/toggles live in TOML + `SETTINGS_SCHEMA`; job creation/listing/cancel is conversational (tool + WebUI), never config. The `[jobs]` section is wired end-to-end per `docs/CONFIGURATION_GUIDE.md` (struct/defaults/parser/`config_to_json`/`config_write_toml`/POST handler/`schema.js`/`dawn.toml.example`), clamped through the shared `config_clamp_jobs()`, and pinned by `tests/test_config_roundtrip.c`. `[jobs]` + `[attention]` sit under the "Scheduling & Background Work" settings category.

---

## 8. Ownership, Authorization & Resource Limits (locked)

Security **invariants**, not optional hardening. Each notes the phase it lands.

1. **Single-user tree (Phase 1).** Child inherits the spawner's `user_id` via `conv_db_create`; the spawn tool never accepts a caller-supplied `user_id`. `is_private` inherited likewise. The derived-join and cascade-cancel rely on this.
2. **Ownership-checked WS handlers (Phase 1/2).** `attach_conversation`, `list_jobs`, `job_action` bind `conn->auth_user_id` and load the target ownership-checked before acting; `list_jobs`/`jobs_snapshot` filtered to the caller. Foreign vs. missing answer identically (no id oracle).
3. **Ownership-JOINed event reads (Phase 2).** Every `conversation_events` read JOINs `conversations` and filters `user_id` (mirrors `conv_db_get_messages`). No naive `WHERE conversation_id=?`.
4. **Resource caps (Phase 1).** Global + per-user + per-provider caps, refuse past cap, bounded per-tick monitor work.
5. **Injection containment (Phase 1 spawn/cancel; hard gate Phase 4 sandbox).** Spawn goals + cancel targets run through the memory injection filter. Structurally, a child controls only its own subtree — by ownership + parent-edge, not prompt. Ingested web/repo content is untrusted.
6. **Secret redaction before persist (Phase 2).** Event payloads pass a **content-based denylist** (`src/core/event_payload.c`) — a sensitive key-name pattern (`pass`/`secret`/`token`/`key`/`credential`/`auth`/`bearer`) or a secret-shaped value (`sk-`, `Bearer `, a long unbroken opaque run) redacts that arg. Deliberately **not** an allowlist (would redact most args and gut the observe surface) and deliberately **not** keyed on `TOOL_CAP_SECRETS`: that capability means a tool *uses* a secrets.toml credential (home_assistant's API token, calendar's OAuth), which lives in config and never appears in the tool's args or results — whole-redacting such a tool only erased benign device/event state, which on a personal assistant is not a secret. `tool_result` bodies are stored **readable** (a tool does not echo its own config credential); the residual — a secret genuinely embedded in freeform result text — is documented and closeable with a substring scrubber if a real case appears.
7. **Output sanitization at render (Phase 2; mandatory Phase 4).** `tool_result`/`terminal_chunk` rendered text-only, control-sequences stripped. The CP4a panel is structurally XSS-proof (`createElement`/`textContent` throughout, `dir="auto"` on LLM-authored `title`/`error`); the transcript render site (CP4b) must uphold the same when it lands.
8. **Events retention/pruning (Phase 2).** Kind-aware: `tool_call`/`tool_result`/`terminal_chunk` payloads NULLed with the row kept; `status` rows deleted; `spawn`/`complete` kept. `event_retention_days` default 30, ON.
9. **Workspace isolation (Phase 4).** `workspace_ref` keyed to `user_id`; `share` inheritance only within one user's tree; reattach validates a generation nonce.

---

## 9. Sequencing vs. the `conversation_history` serialization tech-debt

**Independent, but the phone-broadcaster fix landed first** (contained to `phone_service.c` + a main-loop apply hook) to keep ThreadSanitizer runs clean of a known pre-existing race. No hard dependency, because this design **adds no cross-thread history writers**: job workers write only their own session's history, and parent injection is DB-mediated + idle-dispatched (stated in-code so a future implementer doesn't take the live-inject shortcut).

---

## 10. Phased plan (status)

Hazard map: **H1** → Phase 0 (live) + Phase 2 (durable). **H2/H3** → Phase 1 pool + §4a turn-queue teardown. **Legend:** ✅ SHIPPED · ◑ PARTIAL · ○ AHEAD.

### Phase 0 — Stream inversion + schema (ships P1) — ✅
v72 migration (all columns + `conversation_events` created-but-unused); wire contract frozen with `conversation_id` on deltas + `status`/`complete`/`message_appended` reserved; `conv_stream.c` ring; client delta router + sidebar generating indicator. Conversation captured at dispatch (`stream_conversation_id`, `_Atomic`); fresh-chat pre-create + server back-fill; explicit per-message `conversation_id` with server-side ownership heal; backgrounded final answer persisted server-side. Live-verified.

### Phase 1 — Job pool + lifecycle + authz (ships P2-minimal) — ✅
Separate job-session pool (dual local/cloud counters, cancel-then-wait teardown); cancel/`disconnected` decoupling across all 5 sites incl. compaction; `job_tool.c` (`spawn`/`list`/`status`/`cancel`); `on_complete=notify` via coalesced delivery; dirty-gated heartbeat monitor; boot interrupted-scan; extraction exemption; `max_runtime_sec` reap; v73 `deliver_to` + monitor partial index. §8 invariants 1,2,4,5 in place. TSan-gated. **Simplifications** (→ §14): no queued state machine (fail-fast past cap); `spawn_depth` hardcoded to 1.

### Phase 1.5 — Per-session turn queue + headless workers — ✅ (design addition, §4a)
Turn queue serializing text/voice/reinvoke per session + teardown safety. Headless job workers: the `job` tool is masked from a `SESSION_TYPE_JOB` schema + a headless directive rides the stable prefix + a `handle_spawn` backstop, so a worker does its task inline instead of fanning out. `reinvoke_parent` re-architected onto the queue.

### Phase 2 — Durable event log + observe surface — ◑ (server half + browser observe shipped; full event-render is TUI-only)
- ✅ **Server half:** `conversation_events` writers + per-conversation seq (all kinds); ownership-JOINed reads (§8.3); secret redaction (§8.6, *with the tool_result gap in §14*); kind-aware retention (§8.8); `attach_conversation {last_seq}` replay + `message_appended`; the lifetime-split job frames (§6.4); `job_action {cancel|resume}` + resume re-dispatch + durable `job_goal` (v74); the `tail_conversation.py` line-printer (contract gate). Live-verified on real jobs (event order, seq, `complete` correlation, live tail, 4-job fan-out).
- ✅ **CP4a jobs panel** (`www/js/ui/jobs.js` + `jobs.css`): RUNNING grouped by parent conversation + FINISHED keyset-paginated, live elapsed, per-row View/Cancel/Resume, opened from a header button or the activity pill. Deviations: no depth indent (spawn_depth ≡ 1 today → grouped by parent instead; **trees need an ordering change + a padding rule, both Phase 3**); XSS-safety via `textContent` (stronger than escape-at-site); a job's own conversation reachable only from this panel (job convs are sidebar-hidden).
- ✅ **Watched-job live view:** a running job streams its assistant text **and** live `[Tool Call: …]`/visual entries to any browser viewing its conversation, debug-gated exactly like a normal turn — achieved by admitting `SESSION_TYPE_JOB` to the **normal turn's** stream/thinking/transcript path and fanning those frames out to viewers (`webui_fanout_job_stream_response`), **not** via the event log. This retires the browser's need for the event-log render below: the live view is the streaming path, and reload uses the existing `load_conversation` (persisted messages). Client needed no change — `isForeignConvFrame` already gates on `conversation_id`. The job worker (and the `reinvoke_parent` dispatch paths) install the WebUI turn's `webui_tool_iteration_cb` so the bubble seals at each tool boundary (final answer opens BELOW the last tool, matching a normal turn). Live-verified (text + tools in lock-step, job's own view AND a reinvoked parent). *(A live research job also surfaced pre-existing job-**completion** defects — `max_tokens` mis-marked `done`, duplicate final-answer persist, preamble-soup answer — filed in `docs/TODO.md`; orthogonal to this path.)*
- ✅ **CP4b — completion marker:** viewing a job sends `attach_conversation` (opt-in per request; normal sidebar loads unchanged), and the new `DawnAgentEvents` module (`www/js/ui/agent-events.js`) renders the terminal `complete` disposition at the end of the transcript — "Background job completed / failed: <error> / cancelled / interrupted", live and on reload, text-only (§8.7). `dawn.js` now routes `conversation_event(s)` to it. **Deliberate scope:** the browser draws `complete` (disposition) and `resume` (boundary) from events — kinds NOT in `messages`; content (tool_call/tool_result) stays sourced from `messages` (canonical, survives event pruning — no double-render), and `status` is dropped (transient). The `resume` boundary is placed by `created_at` against the message entries (the load loop tags each with `data-ts`; `insertByTs`). 3-agent review applied (security clean; arch clean; UI WCAG-AA/token/a11y fixes). Live-verified. *(A follow-on: viewing a job live also let the client's streaming backup-save duplicate the server-authoritative final answer — `handle_save_message` now drops client saves for job convs, which are persisted entirely server-side.)*
- ○ **Remaining (browser-side):** the **full event-log render** — drawing `tool_call`/`tool_result` FROM events (not messages) — is **TUI-only future work** (the line-printer has no `messages` store; the browser doesn't need it). **CP5** — live-watch panel (phone-panel twin: server-authoritative elapsed, minimize-to-pill, reconnect rehydrate, sr-only announcer).
- **Verify:** panel survives reconnect with correct replay; the line-printer tails a live job from another machine.

### Phase 3 — reinvoke_parent + trees (ships P3) — ◑
- ✅ **`reinvoke_parent`** (re-architected onto the §4a queue): `max_reinvokes_per_tree` guard, `idx_conversations_parent` derived join, dispatch-only-when-parent-idle, messaging/voice-parent → `notify` downgrade.
- ○ **Trees AHEAD:** multi-level spawn (`spawn_depth = parent+1`), **enforce** `max_spawn_depth`/`max_children_per_tree`/`max_queued_per_user`, ownership-bounded **cascade cancel** + spawn-into-cancelling-tree guard + parent-delete reap, per-conversation settings inheritance, `awaited` UI sugar, SAGE notification coalescing. **Sequencing:** headless workers currently *block* job→job spawning; re-enable **only** behind enforced depth/children caps (correctness cap before the feature). **Prerequisite:** split `on_complete_fired` (§14 known defects) — cascade-cancel adds fire sites to that exact logic.
- **Verify:** parent spawns 2 children → resumes once each (idempotent under induced crash); cancel parent kills children; depth-4 / refire-loop refused.

### Phase 4 — Workspace activation — ○ (gated on the operator-launched sandbox)
`workspace_ref` semantics (MCP server + workspace id + generation nonce); inheritance policy on spawn (share within own tree / fresh / worktree — §8.9); boot reattach with nonce validation; sandbox-count as a second cap dimension; `terminal_chunk` events with sanitization + redaction. **Zero schema/contract change** — the payoff of baking the column + event kind in early.

### Phase 5 — TUI client — ○ (stretch)
Separate program consuming the Phase-2 contract; nothing daemon-side. Blocked on the event contract (now shipped server-side).

---

## 11. Conventions honored

- Return codes `SUCCESS`/`FAILURE` (positive-only); event/insert helpers use `int64_t *id_out`.
- GPL header on new C files; new files under the 1,500 soft limit. Migration bodies in their own `_v7N.c`; only the ladder block touches the size-exempt `auth_db_migrations.c`.
- Layering: `conv_stream.c`/`conv_event.c` L1 leaf (passive; fan-out driven from L4/bridge); job-pool/`job_manager` L2; `job_tool.c` L3; WebUI L4. Downward-only; monitor tick + live fan-out use registered-fn/weak-symbol bridges. `complete` is emitted from the L2 wrapper (`job_manager_set_terminal`), never the DB layer.
- Settings in `dawn.toml` + `SETTINGS_SCHEMA`; job control conversational.
- WS send-funnel invariant preserved; no-ES-modules WebUI conventions (`window.DawnJobs` IIFE; compose `.dawn-badge` + `.dawn-status-dot` state tints — no new chip component; `DawnDialog`/`DawnEscStack`/`DawnStore`/`DawnToast`; phone-panel reuse for live-watch).

---

## 12. Open items / deferred

- `conversation_waits` explicit join table; multi-child *ordered* joins — deferred (derive first).
- Voice/Session-0 turns do not background (they may *spawn* jobs); scoped out. Backgrounded-**voice**-answer server-persist is not mirrored from the text path yet.
- Token-durable logging — rejected (step granularity only).
- True blocking `awaited` — rejected under slot scarcity; re-addable only after a generation-context refactor.
- `seq` MAX+1 → in-memory counter — deferred optimization if contention shows.
- Mixed-provider concurrency accounting edge cases — v1 uses two counters by resolved provider class.

---

## 13. Decisions (locked)

1. Collapse `awaited` → `detached + reinvoke_parent` (UI sugar only).
2. Separate `max_concurrent_local` (default 1) / `max_concurrent_cloud` (default 4) counters.
3. Sidebar nested expander; jobs panel renders the tree (Phase 3); click a running agent → live-tail (CP5).
4. Restart-resume: notification + explicit Resume button; boot never auto-reinvokes a parent.
5. Job conversations do not feed memory extraction; results feed back via the parent.
6. Column name: `parent_id`.
7. Sub-agents get a separate job-session pool (LLM-resource-bound), NOT the audio-bound `MAX_SESSIONS` array.
8. Single-user tree; child inherits spawner `user_id` (+ `is_private`), non-overridable.
9. All new WS handlers ownership-check `conn->auth_user_id`; all event reads ownership-JOINed.
10. Global + per-user job caps + bounded per-tick monitor work.
11. Injection filter on spawn/cancel; a child controls only its own subtree.
12. Secret redaction before persisting event payloads (denylist + capability backstop); untrusted-text rendering for tool/terminal output.

---

## 14. Implementation status & ledger

**Shipped (branch `background-jobs-p2-observe`, on top of P0/P1 merged to `main` via PR #23):**
Phase 0, Phase 1, Phase 1.5, the Phase-2 **server half** + CP4a jobs panel, the **watched-job live view** (a running job streams its text + live tool-call/visual entries to browser viewers via the normal turn's stream/transcript fan-out), and **CP4b** (the terminal completion-disposition marker via `DawnAgentEvents` + `attach_conversation` on job View — §Phase 2). Schema at v74. See the phase table (§10) and git history for per-commit detail.

**Remaining (the single checklist — no scope lost):**

| Item | Phase | State |
|---|---|---|
| **Watched-job live view** — running job streams text + tool calls to browser viewers | 2 | ✅ SHIPPED — via the normal-turn stream/transcript fan-out (`webui_fanout_job_stream_response`), debug-gated like a normal turn; **not** the event log |
| CP4b — `complete`+`resume` markers (`DawnAgentEvents`) + `attach_conversation` on job View | 2 | ✅ SHIPPED — drawn from events (live + reload; `resume` positioned by `data-ts`); content stays from `messages`; `status`/`tool_*` not browser-drawn; client save dropped for job convs (dedup) |
| Full event-log transcript render (tool_call/tool_result FROM events) | 2/5 | ○ AHEAD — **TUI-only** (the line-printer has no `messages`; the browser sources tool content from messages) |
| Jobs completion cluster: deliverable directive, truncation notice, duplicate-persist dedup, `max_tokens` 8192→16384 | 2/— | ✅ SHIPPED — final message IS the result (no document-handoff, ~1500-word target at the reinject budget); a cut-off answer (`max_tokens`/`length`, stashed on `session->last_finish_reason`) gets an "incomplete" notice appended — **status stays `done`**, not a new terminal state; duplicate final-answer row deduped by content. Shared tool-loop `max_tokens` semantics left as-is (rare after the directive). Preamble-soup was the truncation symptom, subsumed. |
| CP5 live-watch panel (phone-panel twin) | 2 | ○ AHEAD |
| Queued state machine + `max_queued_per_user` enforcement + monitor promotion | 2/3 | ○ AHEAD — spawns fail-fast past cap today |
| Trees: `spawn_depth=parent+1`, enforce depth/children caps, cascade cancel, spawn-into-cancelling-tree guard, parent-delete reap, settings inheritance, `awaited` UI sugar | 3 | ○ AHEAD — job→job spawn blocked until caps enforced |
| Workspace/sandbox (`workspace_ref` semantics, MCP reattach, nonce, `terminal_chunk`, sandbox cap) | 4 | ○ AHEAD — column reserved, zero semantics |
| TUI client | 5 | ○ STRETCH |
| Backgrounded-**voice**-answer server-persist (mirror the text path) | — | ○ deferred residual |

**Unused config knobs (shadows of the above):** `max_queued_per_user`, `max_spawn_depth`, `max_children_per_tree` — parsed + round-tripped, zero read sites outside `src/config/`. (`event_chunk_cap` is now enforced — the earlier "unused" listing was stale.)

### Known open defects (authoritative "what's broken now")

Surfaced by the branch review (2026-07-27). A pre-Phase-3 hardening batch addresses several; status is marked per item, and the rest are tracked in `docs/TODO.md`.

1. ✅ **FIXED — `tool_result` secret redaction (was High, §8.6).** Two changes: (a) the emit site now correlates `tool_call_id`→tool name from the batch, so the payload records the real tool (display), and (b) redaction was reworked to be **content-based**, no longer keyed on `TOOL_CAP_SECRETS` — that flag flagged benign-data tools (home_assistant, calendar) whose secret lives in config, and whole-redacting erased device/event state for no gain. Real credentials in args are caught by key-name/value-shape; result bodies are readable. (`src/llm/llm_tool_loop.c`, `src/core/event_payload.c`.)
2. ✅ **FIXED — `delivery_claim` stale-index OOB (was High).** `-1` found-sentinel + insert at `s_delivery_n`; `test_delivery_claim_after_full_table_sweep`. (`src/core/job_manager.c`.)
3. **`on_complete_fired` is one bit answering two questions (Medium — deferred to Phase-3 start).** It gates both "parent re-engaged?" and "user told?"; the delivery path is coherent via the single `job_notify_user()` writer (live-verified), so this is a *structural* hardening, not an active bug. Its whole purpose is to survive Phase-3 cascade-cancel adding fire sites, so it lands at Phase-3 kickoff — next to the code that stresses it, with its own review. Split into `reinvoke_fired` + `notified_at` **before** the trees work.
4. ✅ **FIXED — `event_chunk_cap` minimum clamp (was Medium, memory safety).** `JOBS_EVENT_CHUNK_CAP_MIN` (256) floor in `config_clamp_jobs()` + defensive re-floor in `payload_cap()`; `test_event_chunk_cap_has_a_floor`.
5. ◑ **PARTLY FIXED — event write amplification.** `tool_call`/`tool_result` persistence + fan-out are now **gated to the observe set** (`session->events_observable`), so ordinary interactive turns no longer write step events (live-verified). Remaining: the drain/append still `prepare`/`finalize` per call under the global lock — cache into `s_db.stmt_*` (hardening-batch C2).
6. **Tool-loop honors the *global* interrupt, not the session cancel flag (Medium).** A job wedged in a tool call ignores its session cancel (only the reap's counter-reclaim saves the slot), and a wake-word barge-in / shutdown kills concurrent jobs and files them as "no response from model." Changing it alters cancellation for voice/WebUI/messaging alike, so it's its own change.
7. **UI (Low/Medium):** the 1-s live rebuild drops keyboard focus from View/Open buttons; the observe panel has no `aria-live` region. `jobs.js` is 952 lines (approaching the 1,000 JS soft limit before CP4b).
8. **Files over limits:** `session_manager.c` is 2,855 lines (over the 2,500 hard limit; pre-existing, drifting). Extract the job-pool-integration block on next touch.

### Fold-ins carried on this branch (not core-feature, enumerated for accounting)

These are correct, low-regression fixes surfaced during job testing that touch shared or unrelated code:
- **Shared LLM/tool hardening:** UTF-8 sanitize of byte-truncated tool summaries in `llm_claude_format.c` (protects every Claude turn / restored history); advertise the `document_manage` save-text budget in `llm_tools.c`. (Root cause of the resume-crash is the history loader dropping `tool_call_id` — filed in TODO.)
- **Memory:** job-transcript extraction guard in `should_skip_memory_extraction()`; the private-`remember` confirm gate; **`valid_to == valid_from` relation-validity acceptance** + the memory-panel validity-range UI (a memory fix *unrelated* to jobs).
- **Settings/build:** un-hide the `[jobs]` **and `[attention]`** panels (`SECTION_CATEGORIES`) + rename the category; `-Wswitch` globally + one pre-existing `WS_RESP_REASONING_SUMMARY` completeness fix; the `DawnJobs.close()` cross-close line in `memory.js`.
