# Durable Background Jobs — Design

**Status:** PARTIALLY SHIPPED — committed per the design-doc policy now that Phase 0 + Phase 1 are built and the
implementation matches this doc substantially. It stays a **living** doc while Phases 2-5 are outstanding; the
historical record of the shipped subset graduates to `atlas/dawn/archive/` once the program completes.

> **Note for reviewers:** `docs/TODO.md` and `docs/DONE.md` are developer-local planning files (gitignored), so
> references to them below won't resolve in a checkout. Deferred review findings are summarised in §14's ledger,
> which is self-contained.

- **SHIPPED — merged to `main` 2026-07-24 via PR #23.** Phase 0 (`15a838e`) + Phase 1 / reinvoke / turn-queue /
  headless (`2bbe683`) + the first *observe*-side sidebar indicators (`f42fce1`), then the post-review follow-ups:
  the **`max_runtime_sec` runtime reap** (`1ae36d9`), the **`[jobs]` settings round-trip + TOML-string escaping**
  (`b7d21dd`), and a **CI fix** so `test_job_manager` compiles on the WEBUI-off preset CI runs (`80f890a`).
  `98bc048` (phone single-owner) was the prerequisite; `c681e32` (Claude adaptive thinking) and `a18c2bd`
  (sidebar date-grouping) rode along unrelated. Together: the P1 streaming primitive, the Phase-1 job pool +
  `job_tool` + lifecycle + authz, `reinvoke_parent` **re-architected through a new per-session turn queue** (a
  design addition not in the original §10 plan — see §4a/§14), headless job workers (job tool masked + headless
  prompt so workers don't fan out), the memory-extraction exemption, and the sidebar **done/unread dot** +
  **per-conversation running-jobs pill** + **active-view `metrics_update` gating** (a Phase-2-observe precursor;
  the full jobs panel/tree/live-watch is still AHEAD). §14 is the authoritative shipped-state record.
- **AHEAD (this doc's forward plan)** — Phase 2 (durable event log + observe UI), the **trees** half of Phase 3
  (multi-level spawn + depth/children caps + cascade cancel), the **queued state** + **Resume**, Phase 4
  (workspace/sandbox), Phase 5 (TUI). Nothing from the original scope is dropped — §10 marks each item's state;
  §14's "Designed-but-not-yet-shipped ledger" is the single checklist so no scope is lost.
- Authored 2026-07-22; shipped-state reconciliation 2026-07-24.

**Reviewers:** master-plan-reviewer (plan pass); architecture + security + ui-design (doc pass); a 6-agent full
pre-commit review of `2bbe683` (architecture / security / embedded-efficiency / coding-standards / UI /
master-code-reviewer); and a 4-agent review of the reap + config-wiring follow-ups (architecture / security /
embedded-efficiency / coding-standards) whose findings reshaped both before commit — see §5 and the DONE.md rows for
`1ae36d9` / `b7d21dd`. All Critical/High/Medium fixed; defer-grade items are summarised in §14 (and tracked in the developer-local `docs/TODO.md`).

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

## 2. What already exists (reuse) vs. what is new

Verified against the code (three read-only passes + a doc-review verification pass, 2026-07-22).

**Reuse — DAWN already owns these:**

- Conversations are **session-independent**, durably persisted, created/loaded/searched/deleted by `(conv_id, user_id)` — `conv_db_*` in `src/auth/auth_db_conv.c`. A conversation exists with no live session.
- A **self-FK precedent** already exists: `continued_from INTEGER → conversations(id) ON DELETE SET NULL` (`auth_db_schema.c:154,185`). `parent_id` is a sibling edge (kept distinct — `continued_from` = compaction lineage).
- **Generation is bound to the session, not the fd**: the chunk callback captures a session pointer; the socket is resolved at emit time from `session->client_data` (`webui_send.c:764`), so reconnect re-homes a live stream.
- **No global gate on the WebUI path**: `llm_processing`/single `llm_thread` is voice/Session-0 only (`dawn.c:256-257,3903`). WebUI/DAP2/messaging already spawn **one detached worker per request** keyed to a session, with an atomic `request_generation` supersede counter (`webui_text_processing.c:661`).
- Switching conversations already does **not** abort the in-flight turn.
- The **ownership-JOIN pattern** for authorization already exists: `conv_db_get_messages` binds `user_id` via a JOIN (`auth_db_conv.c:1503-1540`); every WS conv handler binds `conn->auth_user_id` (`webui_history.c`). The new surfaces reuse this exactly.
- Completion **delivery** (chime/banner/voice/`deliver_to` channel) is built in scheduler/briefing + messaging Phase 5; SAGE attention layer already has notification budgets/quiet-hours to coalesce against.
- The main-loop **1-second heartbeat** already drives periodic work with no dedicated thread (OTA rollout precedent).

**New:**

- v72 schema columns + a `conversation_events` step-log table.
- `conversation_id`-tagged deltas + a conversation-keyed in-memory replay ring (`src/core/conv_stream.c`, Layer-1 leaf).
- A **separate job-session pool** (own storage + lifecycle) — not the interactive `sessions[8]` array (§4).
- `src/tools/job_tool.c` (LLM tool) + a WebUI jobs surface (`www/js/ui/jobs.js`).
- A completion monitor tick on the existing heartbeat.
- `reinvoke_parent` DB-mediated follow-up with budgets + cascade cancel.

---

## 3. Data Model

A job is a `conversations` row with `parent_id` set (or, for a root user-initiated job, `job_status` set with `parent_id` NULL). New columns (all **nullable / literal-defaulted** → fast ALTER, no table rewrite):

| Column | Type | Meaning |
|---|---|---|
| `parent_id` | INTEGER, FK → conversations(id) ON DELETE SET NULL | Who spawned me. NULL = root/user-initiated. Distinct edge from `continued_from`. |
| `spawn_mode` | TEXT | `detached` only in v1. (`awaited` is UI sugar rendered onto detached+reinvoke — §5. Column exists for forward-compat/presentation.) |
| `on_complete` | TEXT | `notify` \| `reinvoke_parent` \| `none`. |
| `on_complete_fired` | INTEGER DEFAULT 0 | Idempotency flag (Odysseus `followed_up`). Set **only after** the notify/reinvoke succeeds. Cancel sets it to suppress follow-up. |
| `job_status` | TEXT DEFAULT NULL | NULL = **not a job**. Else `queued` \| `running` \| `done` \| `failed` \| `interrupted` \| `cancelled`. Orthogonal to `is_archived`/`is_private`. |
| `job_error` | TEXT DEFAULT NULL | Failure reason (sanitized — §Security). |
| `spawn_depth` | INTEGER DEFAULT 0 | 0 = root. *(v1 shipped: hardcoded to 1 on create; `= parent+1` propagation + the `max_spawn_depth`/`max_children_per_tree` caps are the Phase-3 trees work — §14 ledger.)* |
| `reinvoke_count` | INTEGER DEFAULT 0 | Per-tree cumulative re-dispatch counter (livelock guard, §5/§Security). |
| `started_at` | INTEGER DEFAULT 0 | Job run start (epoch). |
| `finished_at` | INTEGER DEFAULT 0 | Job terminal time (epoch). |
| `workspace_ref` | TEXT DEFAULT NULL | Opaque handle to the sandbox (MCP server id + workspace id + generation nonce). Semantics land in Phase 4; column reserved now. |
| `deliver_to` | TEXT DEFAULT NULL | *(added in **v73**, not v72 — this table originally missed it.)* Completion-notification target for the `notify` path, so a messaging-origin job answers back on its own channel (same semantics as scheduler `deliver_to`). Bound by `conv_db_create_job`; read by the monitor's notify batch. |

`user_id` is **not** added — the child inherits the spawner's `user_id` via `conv_db_create` and every read is ownership-JOINed (§Security). A tree is single-user by invariant.

**Fan-out/join is derived, not stored** — "parent resumable iff it has no children with `job_status='running'` AND `on_complete='reinvoke_parent'`" is one index-assisted scan over the (≤4) child set. New index `idx_conversations_parent (parent_id, job_status)`. A `conversation_waits` table is added only if multi-child *ordered* joins ever materialize (deferred).

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

- **No `user_id` column** (mirrors the `messages` table); every read JOINs `conversations` and filters `user_id` (§Security C2).
- `seq` starts as `MAX(seq)+1` under the auth_db leaf mutex; if contention shows at step granularity, switch to an in-memory per-conversation `next_seq` counter in `conv_stream` that persists its value (deferred optimization).
- Final assistant text is **not** duplicated here — it lives in `messages`. The `complete` event carries `final_message_id` **and** the finished text is pushed as a `message_appended` frame so a pure tailer gets the answer (§6, U-Crit).

**Migration:** the v72 body goes in a **new `src/auth/auth_db_migrations_v72.c`** (matching `_v67.c`); only the ~15-line AND-gated ladder block is appended to the size-exempt `auth_db_migrations.c`. Schema version bumps `71 → 72` (`include/auth/auth_db_internal.h:59`). `conversation_events` is created in v72 but unused until Phase 2. Follows the `conv_column_exists()` probe + literal-DEFAULT discipline of `auth_db_migrations_v67.c`.

*As-built:* v72 shipped the **11** columns above (`parent_id` … `workspace_ref`); a follow-on **v73** (`auth_db_migrations_v73.c`) added the 12th column **`deliver_to`** plus the monitor's partial index `idx_conv_job_followup`. **Current schema version is 73** (`AUTH_DB_SCHEMA_VERSION`).

---

## 4. The three hazards and their resolution

1. **Output is dropped when unattached.** Today chunk callbacks early-return on `session->disconnected` (`session_manager_llm.c:64`) and queued responses are freed undelivered (`webui_send.c:716-719`). **Fix (the core inversion):** deltas are published to a conversation-keyed in-memory replay ring (`conv_stream.c`) and tagged with `conversation_id`; step transitions persist to `conversation_events`. A client attaching (or on another conversation) replays from the ring + durable events. Live token emission still gates on an attached client, but content is no longer lost — it is replayable (ring) and its steps durable (events).

2. **8-session ceiling — but that ceiling is audio-bound and does not apply to sub-agents.** The `MAX_SESSIONS 8` cap (`session_manager.h:88`) exists because each interactive session is **audio-pipeline-bound** (a Whisper/Vosk recognizer, TTS engine access, an audio worker). Correction to the earlier draft: the code allocates *all* current session types from one global `sessions[8]` array via `find_free_slot()` (`session_manager.c:280-288`), and messaging's `MESSAGING_MAX_SESSIONS=64` is only an LRU *directory over those 8 slots* — it is **not** a separate pool. **Fix:** a background job is text-only (LLM tool-loop + MCP; no ASR/TTS/audio), so it gets a **genuinely separate job-session pool** — its own `session_t` storage array + its own `session_create_job` / `find_free_job_slot` / **`session_get_job_by_id`** (the monitor/cancel path needs a lookup over the job pool) / job-pool destroy — and is **not** counted against `MAX_SESSIONS`. Reuse: the `session_t` *type* and every worker/streaming/orchestration path that takes `session_t*` are unchanged (`retain`/`release` operate on `session_t*` directly). The job pool's cap is **LLM-resource-bound**, driven by the `max_concurrent_local`/`max_concurrent_cloud` counters (§7) plus the security caps (§Security), never by the audio budget. Consequence: a running job can **never** starve an interactive browser/satellite (different array), and the interactive `session_destroy` path is untouched by the job lifecycle. **Phase-1 footprint check:** sizing the job array to `max_active_jobs` × full `session_t` means each idle job slot carries interactive-only fields (audio/ASR/metrics) it never uses — confirm `session_t` embeds no large static PCM/ring scratch buffers; if it does, use a slimmer job-session variant or lazy-allocate the audio members.

3. **Ref-count 3s-timeout UAF** — interactive `session_destroy` waits 3s for `ref_count==0` then proceeds anyway (`session_manager.c:928-935`). Because jobs live in a **separate pool** (Hazard 2), their teardown is a **new** path: a job worker holds a ref for its whole run; job-pool teardown is **cancel-then-wait** (set `cancel_requested`, worker observes and unrefs, *then* free) with no "proceed anyway." The interactive path keeps its existing 3s behavior. `session_get_local()`'s no-ref return stays a documented voice-only exception.

**No new cross-thread history writers.** Job workers write only their *own* session's history (existing `history_mutex`). Parent follow-up is **DB-mediated and dispatched only when the parent is idle** (§5) — never a cross-thread `session_add_message` into a live session. This composes with the open `conversation_history` serialization fix (§9).

**Cancel/`disconnected` decoupling (interactive path, for P1 disconnect-survival):** to let an interactive backgrounded turn survive a tab close, live-turn cancellation moves to a new per-session `cancel_requested` atomic; `disconnected` then gates emission only. This touches **all five** `llm_set_cancel_flag(&session->disconnected)` sites — 3× `session_manager_llm.c` (streaming) **and** `llm_context.c:1832` (async compaction) — because `session_destroy` joins the compaction thread relying on `disconnected` aborting its CURL (`session_manager.c:909-919`). Invariant: `session_destroy` sets **both** `disconnected` and `cancel_requested`. A destroy-during-compaction ThreadSanitizer test is required in Phase 1.

---

## 4a. Per-session turn queue — the reinvoke concurrency foundation (SHIPPED, not in the original plan)

The original design (§5) re-engaged a parent by injecting into it and re-dispatching directly. When `reinvoke_parent`
was actually built, streaming it into the user's **live** viewer session raced the user's **own** turn on that same
`session_t` — TSan-confirmed data races in `session_text_chunk_callback` / `llm_streaming` (two turns stomping one
session's per-turn streaming state). `request_generation` turned out to be a post-hoc "who persists" arbiter, **not a
lock**. The fix generalized into infrastructure the whole feature now rests on:

- **`src/core/turn_queue.c` (Layer-1 leaf).** A per-session FIFO that guarantees **at most one turn runs on a given
  `session_t` at a time**. All three WebUI turn producers — user **text** (`webui_text_processing.c`), **push-to-talk
  voice** (`webui_audio.c`), and **background reinvoke** (`job_reinvoke.c`) — enqueue instead of `pthread_create`-ing
  the worker directly. "Two turns never touch one session's streaming state concurrently" becomes a *structural*
  property, not a race to manage. One leaf mutex, never held across the spawn/free closures; `TURN_SOURCE_USER` (cap 16)
  vs `TURN_SOURCE_BACKGROUND` (cap 8).
- **Reinvoke rides the queue.** `reinvoke_run` finds+retains the live viewer and **enqueues a marker**
  (`reinvoke_queued_t`) as a background turn (or falls back to a detached job-pool session when there's no viewer). The
  envelope is built at **dequeue** (re-querying the parent's pending jobs so late siblings coalesce), and there is an
  **exactly-once release matrix** for `{session_release, inflight_release, mark_dirty, free}` across every path
  (enqueue-OK / FULL→detached / calloc-fail / spawn-fail / `being_destroyed`-at-dequeue / purge / run). C3
  server-persist-before-mark-fired: a re-engagement is marked fired only once its reply is actually saved (client-saved
  foreground, or server-persisted when backgrounded/client-gone).
- **Teardown safety (closes Hazard 3 for the interactive path).** `session_t.being_destroyed` (set first in
  `session_destroy`) + a slot `closing` flag + purge-before-ref-wait; `session_begin_turn_flags` **no-ops under
  being_destroyed** so a turn spawned into the tiny pop-to-run window can't resurrect the teardown cancel. The
  interactive `session_destroy` ref-wait became **bounded-retry-then-leak-don't-free** (`SESSION_DESTROY_REF_WAIT_MAX_SEC`
  = 30) — leaking a few-KB `session_t` is recoverable; freeing it under a still-running worker is a UAF. This supersedes
  the original Hazard-3 "interactive keeps its 3s proceed-anyway" note. `session_t.last_activity` became `_Atomic`
  (it was written under `history_mutex` but read under `session_manager_rwlock` — mismatched locks).

**Producer coverage note:** the DAP2 satellite `satellite_query` path is the one remaining turn producer NOT yet on the
queue (distinct session objects + reinvoke only targets WEBUI sessions, so the invariant holds today). Route it through
the queue when a third concurrent producer per satellite session appears. Design record: `~/.claude/plans/per-conversation-turn-queue.md`.

---

## 5. Lifecycle

> **Shipped-state note (2026-07-24):** the lifecycle below is the design intent. Three things diverged in the shipped
> build (§14): (1) `reinvoke_parent` re-engages via the **turn queue** (§4a), not a direct DB-inject-and-redispatch;
> (2) there is **no queued *state machine*** — `conv_db_create_job` does insert the row as `job_status='queued'` and
> `job_tool` cancel handles a still-queued row, but that status is only the momentary pre-dispatch value the worker
> immediately promotes to `running`; nothing ever *parks* in `queued`, there is no monitor promotion, and over-cap
> spawns **fail fast** instead of enqueueing (so `max_queued_per_user` is unenforced); (3) `spawn_depth` is
> **hardcoded to 1** at the `job_tool.c` create call (real depth + caps are the Phase-3 trees work).
> Everything else (spawn gating, `detached`+`reinvoke_parent`, completion monitor, cancel, restart-interrupted,
> extraction exemption, **and the `max_runtime_sec` reap** — see below) shipped as written.

**Runtime reap — as-built (2026-07-24, post-review).** `job_manager_reap_overdue(now)` runs from `jobs_monitor_tick`
on the 1-Hz heartbeat, deliberately **ahead of the dirty-gate**: a wedged job never reaches a terminal state, so it
never marks anything dirty — gating the reap on that flag would blind it to the exact case it exists for. It walks the
**in-memory pool only** (each slot carries its own `started_at`, so no DB read on the voice loop) and early-outs
when nothing is running, keeping the idle tick at zero DB work (measured: 12.4 ns idle, 643 ns with 16 slots busy).

**A reap is a cancel *request*, not a kill — so it escalates in three stages:**

1. **First strike** — flag the slot (`reaped_at`) and `session_cancel_turn()`. Log says *"cancel requested"*, not
   "marked failed", because this function only asks: the **worker** writes the terminal state and the **monitor**
   fires the follow-up, and neither happens if the job never returns.
2. **Nag** (every `JOB_REAP_NAG_INTERVAL_SEC`) — re-issue the cancel and re-log, so a stuck job stays visible
   instead of producing exactly one line and then going silent forever.
3. **Force-release** (after `JOB_REAP_FORCE_RELEASE_SEC`) — reclaim the job's **provider counter**. Cancellation is
   only observed where the session flag is polled (the LLM/CURL transfer); a job wedged inside a **tool call** never
   sees it, because the tool loop's per-iteration gate checks the *global* wake-word interrupt rather than the
   session flag (verified — tracked as its own TODO, since fixing it changes cancellation for every caller). Without
   this stage such a job holds `max_concurrent_local` (default **1**) forever, i.e. exactly the DoS the reap exists
   to close. The **slot** stays owned by the zombie so its `session_t` pointer remains valid; only the scarce counter
   is reclaimed, and `job_manager_end()` skips its decrement (`counters_released`) if the worker ever does return.

**Disposition — the claim, not a query.** `job_manager_claim_reaped()` both reports the verdict *and* stops the
reap clock, because the slot would otherwise stay reapable across the worker's whole terminal-write block (several
DB round-trips). The worker snapshots `cancel_requested` **before** claiming — a reap raises that flag too, so
reading it after would let a reap masquerade as a user cancel and silently swallow the timeout's follow-up. It then
**persists any produced answer regardless of disposition**: a job that finished microseconds before its deadline did
the work, and reporting it as "timed out before producing a result" while discarding the answer is the worst
available outcome. Precedence is therefore: user-cancel (suppress notification, keep the answer) → answer produced
(`done`, even if the reaper fired) → reaped with nothing (`failed` + `JOB_ERR_TIMED_OUT`, follow-up **still owed**,
no `mark_fired`) → empty response.

The tree-budget requirement needs no extra code: a reaped `reinvoke_parent` row flows through the normal reinvoke
path, where `conv_db_job_bump_reinvoke` already charges it against `max_reinvokes_per_tree`, so timeout →
re-dispatch → re-spawn cannot run forever. `job_record_timed_out()` is the single definition of the "failed +
timed out" test, so the notice wording and the reinvoke envelope cannot drift. `max_runtime_sec <= 0` disables
reaping; a backwards clock step (NTP) yields a negative elapsed and reaps nothing.

**Pool sizing (changed 2026-07-24).** The slot array is allocated **once to a fixed `JOB_POOL_MAX_SLOTS` (256)**,
*not* to `max_active_jobs`. Wiring `[jobs]` into the settings panel made that field runtime-mutable, and a
boot-sized array would then let a raised cap pass `job_manager_capacity()` (a counter comparison) while
`job_manager_begin_ex()` found no free slot — a phantom "job capacity reached" failure on a job row that had already
been created. `max_active_jobs` is now a pure policy counter with nothing allocated from it; the constant must stay
≥ the clamp ceiling in `config_clamp_jobs()`.

### Spawn

Jobs are created via an **LLM tool** (`job_tool.c`, actions `spawn`/`list`/`status`/`cancel`) and the WebUI — never TOML (JARVIS conversational-control principle). Spawn:

1. Creates a child conversation (`conv_db_create`) with the **spawner's `user_id` (non-overridable)**, `parent_id`, `spawn_mode`, `on_complete`, `spawn_depth = parent.spawn_depth + 1`, `job_status='queued'`, inheriting the parent's per-conversation LLM settings by default (explicit overrides allowed — messaging seeded-settings precedent).
2. **Budget gates — refuse (not silently enqueue) past a cap:** `spawn_depth ≤ max_spawn_depth`; active children in this tree `≤ max_children_per_tree`; **global** active jobs `≤ max_active_jobs`; **per-user** concurrent `≤ max_jobs_per_user` and queued `≤ max_queued_per_user`; per-provider running counter (`max_concurrent_local`/`_cloud`). If the cancelling ancestor's `cancel_requested` is set, spawn is refused (spawn-into-cancelling-tree guard). Over the running-counter but under the queue cap → `queued`; over the queue cap → refusal returned to the LLM.
3. When a job-pool slot + provider counter are available → `job_status='running'`, `started_at=now`, emit `status` + `spawn` events.

### `awaited` presented, `detached` executed

`awaited` is **not** a blocking code path (a parked parent would waste a job slot and risk starvation). Instead the parent's turn **ends**; `on_complete='reinvoke_parent'` makes the child's completion re-dispatch the parent. The UI may *present* this as "waiting for subtask," but no slot ever blocks.

### Completion + follow-up (monitor on the 1s heartbeat — no new thread)

Each tick drains up to **`monitor_followups_per_tick`** rows (bounded work — never the whole backlog, so the voice-servicing main loop isn't degraded) where `job_status IN (done,failed,interrupted) AND on_complete_fired=0`:

- `on_complete='notify'` → deliver via scheduler/briefing infra (coalesced — §6 completion), `deliver_to` for messaging-origin jobs. Flip `on_complete_fired=1` **only on success**; retry next tick otherwise.
- `on_complete='reinvoke_parent'` → **defer if the parent has a turn in flight** (Odysseus monitor discipline). When parent idle: load the child result from `messages` (`conv_db_get_messages`, ownership-scoped), inject into the parent conversation (DB + `session_add_message` + `session_dispatch_user_turn`), re-dispatch, `reinvoke_count++`. Flip the flag only after dispatch succeeds. If `reinvoke_count ≥ max_reinvokes_per_tree`, stop re-dispatching and notify instead (livelock guard).
- A `max_runtime_sec` reap marks a hung job `failed` and **still fires** its follow-up ("timed out") so completion never silently no-ops — but the reap counts against `reinvoke_count`/the tree budget so a timeout→re-dispatch→re-spawn loop cannot run forever.

### Cancel (ownership-bounded, cascading)

Cancel sets the target's per-session `cancel_requested`, marks `job_status='cancelled'`, sets `on_complete_fired=1` (suppress follow-up), and **cascades** to `running` descendants via a depth-bounded recursive walk **strictly filtered `WHERE user_id = caller`**. Best-effort MCP cancel is sent to the sandbox; DAWN cancels its *conversation*, not the operator-owned remote process (documented limitation). A child spawned into a cancelling tree is refused (spawn-time `cancel_requested` check) so a spawn-races-cancel can't orphan a runner. Deleting a parent with running children reaps/cancels them in the `conv_db_delete` path **before** the FK nulls the edge (else `ON DELETE SET NULL` orphans them from the cascade walk).

### Restart semantics (honest, structural)

The job body dies with the daemon (no-subprocess rule). On boot: scan `job_status='running'` → set `interrupted` + `job_error`, **and for `reinvoke_parent` jobs set `on_complete_fired=1`** so the heartbeat monitor never *auto-reinvokes a parent across a restart* (that would be the silent auto-resume decision #4 rejects). Queue a notification. If `workspace_ref` is present (Phase 4), attempt MCP reattach only after validating its generation nonce. **Resume is an explicit user action** (a Resume button re-dispatches with history + a "you were interrupted" system line). No seamless survival is promised.

### Memory extraction

Child/job conversations (`job_status IS NOT NULL`) are **exempt** from session-end memory extraction by default (the notes-store `note_extraction_guard` lesson — avoids poisoning the fact store with tool noise). Job *results* still reach memory the right way: they feed back into the **parent** via `reinvoke_parent`, and the parent's normal extraction captures what matters.

---

## 6. Stream & observe contract (WebUI **and** future TUI)

One attach protocol, two renderers. All server→client frames go through the `send_json_response`/`queue_response` funnel (CI-enforced) — including live fan-out to a second attached client.

**Server → client:**

1. **`delta`** — existing streaming frames, **+ a `conversation_id` field**. Ephemeral live tokens (carry `stream_id`); never persisted; replayable only from the in-memory ring.
2. **`conversation_event`** `{conversation_id, seq, kind, payload, created_at}` — durable steps. `kind` ∈ `status` (turn/job state — see below), `tool_call`, `tool_result`, `terminal_chunk` (Phase 4), `spawn` (child conv_id + title), `complete`. This is the stream a dumb line-printer TUI tails.
   - **`status` is emitted for EVERY conversation, not just jobs** — `{state: generating|idle}` at turn start/end — so the sidebar chip and a TUI read one durable signal (not delta-timing).
   - **`complete` carries terminal disposition**: `{disposition: done|failed|interrupted|cancelled, error?, final_message_id}` so a tailer renders the ending from the stream alone.
3. **`message_appended`** `{conversation_id, message_id, role, text}` — pushed when a turn's final assistant message persists, so a pure event-tailer receives the **answer body** (which lives in `messages`, not events). Both renderers append identically. *(Without this the Phase-2 TUI verification cannot pass — U-Crit.)*
4. **`jobs_snapshot`** / **`job_update`** `{conv_id, parent_id, title, job_status, spawn_depth, started_at, last_event_seq}` — list-level state for badges/panels. Pushed/requested on `(re)connect` (mirrors `phone_status`).

**Client → server:**

- **`attach_conversation {conv_id, last_seq}`** (extends the existing `load_conversation` handler): server **binds `conn->auth_user_id`, loads the conv ownership-checked** (`conv_db_get(conv_id, auth_user_id)`), then replies with (a) messages via `conv_db_get_messages`, (b) durable events where `seq > last_seq` **JOINed on `user_id`**, (c) in-memory ring replay of the current partial turn, then (d) live-tails. **Register-subscriber-before-replay** + seq-dedup + end-sentinel tail-flush (copied from `agent_runs.subscribe()`). Replay size capped / rate-limited (L1).
- **`list_jobs`**, **`job_action {spawn|cancel|resume, conv_id, ...}`** — mirror `phone_action`'s shape **and** ownership-check `conv_id` against `auth_user_id` before acting (IDOR guard — §Security C3).

**Ordering unifier:** within a conversation, `messages.id` and `conversation_events.seq` interleave by insertion order; live deltas belong to the not-yet-persisted tail message. Renderer algorithm (identical both surfaces): render merged messages + events → append partial tail from ring replay → live-tail → append final text on `message_appended`.

**Layer contract for `conv_stream.c`:** a passive **Layer-1 leaf** store keyed by `conversation_id` (leaf mutex, copy-under-lock, no I/O or send-funnel call while held). Higher layers **pull** on attach. Live fan-out to a *second* attached client (e.g. TUI + browser on the same conversation) is driven from the **emit path (Layer 4)** or the registered-fn bridge used for the monitor — never a Layer-1→Layer-4 upcall. (The common P1 case — one browser multiplexing all conversations — needs no cross-session fan-out; the client delta-router keys on `conversation_id`.)

**Untrusted-content discipline (both renderers):** `tool_result` and `terminal_chunk` payloads are attacker-controllable (web fetches, compiled/run output). They are persisted secret-redacted (§Security H4) and rendered **as text only** — WebUI via `textContent`/DOMPurify, control-sequences stripped; TUI strips ANSI/control bytes. `conversation_events.payload` is untrusted at every read site.

---

## 7. Configuration (`dawn.toml`)

```toml
[jobs]
enabled = true
max_concurrent_local = 1        # single GPU: keep to 1 with a local LLM (LLM-resource-bound, NOT audio-bound)
max_concurrent_cloud = 4        # cloud provider: parallelism is cheap
max_active_jobs = 16            # GLOBAL ceiling across all users/trees (slot + cost DoS guard)
max_jobs_per_user = 4           # per-user concurrent running
max_queued_per_user = 8         # per-user bounded backlog; reject beyond (no silent enqueue)
monitor_followups_per_tick = 4  # bound heartbeat work so the voice loop isn't degraded
max_spawn_depth = 3             # tree depth cap
max_children_per_tree = 4       # active children per parent
max_reinvokes_per_tree = 8      # livelock guard on reinvoke_parent / timeout-refire
max_runtime_sec = 1800          # per-job reap (still fires follow-up, counts against reinvoke budget)
event_chunk_cap = 16384         # head+tail truncation for tool_result / terminal_chunk payloads
```

> **As-built (2026-07-24):** the whole block parses + clamps, but **4 of the 13 knobs have no read site outside
> `src/config/`** — `max_queued_per_user`, `max_spawn_depth`, `max_children_per_tree`, `event_chunk_cap`. They are
> forward-declarations for Phases 2/3 (§14 ledger); setting them in `dawn.toml` today changes nothing.
> `max_runtime_sec` **is** now enforced (the reap, §5) — `0` disables it.
>
> **Settings round-trip (fixed 2026-07-24).** `[jobs]` originally shipped wired into only 4 of its 8 touchpoints —
> struct, defaults, parser, and the WebUI *client* `schema.js` — with **no server-side support at all**. Because a
> WebUI settings save rewrites the whole `dawn.toml` from the in-memory config
> (`webui_config.c` → `config_write_toml`), and the writer never emitted `[jobs]`, **the entire section was deleted
> from the user's `dawn.toml` on any settings save**, silently reverting every job cap to its default; the panel also
> showed defaults rather than live values, and edits were dropped on POST. Now wired end-to-end:
> `config_to_json` (all 13 fields), the POST handler (all 13, clamped through the new shared `config_clamp_jobs()` so
> the wire path can't bypass the file path's bounds), `config_write_toml` (all 13 — the writer emits from the
> in-memory config, so writing even the not-yet-enforced knobs is what preserves a hand-edited value across a save),
> and `dawn.toml.example`. The panel deliberately surfaces only the 8 meaningful knobs; the 4 Phase-2/3 forward-decls
> stay out of the UI (a control that does nothing is worse than no control) but still round-trip.
> `tests/test_config_roundtrip.c` now pins this in CI.

Two **independent** provider counters: a job is counted against `max_concurrent_local` or `max_concurrent_cloud` by its **resolved provider class** (a per-conversation setting can make a child cloud under a local default). The job-pool storage array is sized to `max_active_jobs`. Caps/toggles live in TOML + `SETTINGS_SCHEMA`; job creation/listing/cancel is conversational (tool + WebUI), never config.

---

## 8. Ownership, Authorization & Resource Limits (locked)

These are **security invariants**, not optional hardening — the events table's missing `user_id` and the "mirror phone_action" wording otherwise make the *unsafe* path the easy one. All must be in place by the phase noted.

1. **Single-user tree (Phase 1).** A child inherits the spawner's `user_id` via `conv_db_create`; the spawn tool never accepts a caller-supplied `user_id`. A tree is entirely one user's. The derived-join and cascade-cancel rely on this.
2. **Ownership-checked WS handlers (Phase 1).** `attach_conversation`, `list_jobs`, `job_action` bind `conn->auth_user_id` and load the target conv ownership-checked before acting. `list_jobs`/`jobs_snapshot` filtered to the caller (no cross-user title/topology leak).
3. **Ownership-JOINed event reads (Phase 2).** Every `conversation_events` read JOINs `conversations` and filters `user_id` (mirrors `conv_db_get_messages`). No naive `WHERE conversation_id=?`.
4. **Resource caps (Phase 1).** Global `max_active_jobs`, per-user concurrent + queued caps, bounded queue (reject past cap), per-tick monitor bound, per-provider fairness so one user can't monopolize the shared cloud counter.
5. **Injection containment (Phase 1 spawn/cancel; hard gate Phase 4 sandbox).** Run spawned job goals and `cancel` targets through the existing memory injection filter before acting; **structurally**, a child job may only control (cancel/reinvoke) its own subtree — enforced by ownership + parent-edge, not by prompt. Ingested web/repo content is untrusted.
6. **Secret redaction before persist (Phase 2).** Event payloads pass through the log-redaction machinery (`ENV_SECRET` discipline) with an allowlist of safe tool args; never persist raw `Authorization`/secret-bearing fields or `job_error` detail.
7. **Output sanitization at render (Phase 2; mandatory Phase 4).** `tool_result`/`terminal_chunk` rendered as text only, control-sequences stripped (§6).
8. **Events retention/pruning (Phase 2).** Terminal-state + age policy prunes `terminal_chunk`/`tool_result` payloads (keep the row) so durable, secret-risk-bearing output doesn't accumulate forever on a Jetson. **Also age out `status`/`spawn`/`complete` rows** — because §6.2 emits a durable `status` event at turn start/end for *every* conversation (2 rows/turn on all interactive conversations, not just jobs), long-lived interactive conversations would otherwise grow the events table unbounded.
9. **Workspace isolation (Phase 4).** `workspace_ref` keyed to `user_id`; `share` inheritance only within one user's own tree; reattach validates a generation nonce before resuming (stale/recycled id can't reattach to a foreign workspace).

---

## 9. Sequencing vs. the `conversation_history` serialization tech-debt

**Independent, but land the phone-broadcaster fix first.** It's small (contained to `phone_service.c` + a main-loop apply hook) and keeps Phase 1's ThreadSanitizer runs clean of a known pre-existing race. No hard dependency, because this design **adds no cross-thread history writers**: job workers write only their own session's history, and parent injection is DB-mediated + idle-dispatched. State this in-code so a future implementer doesn't take the live-`session_add_message`-into-parent shortcut.

---

## 10. Phased plan (status-annotated 2026-07-24)

Hazard map: **H1** → Phase 0 (live) + Phase 2 (durable). **H2/H3** → Phase 1 pool + §4a turn-queue teardown. Security invariants (§8) land at the phase each notes. **Legend:** ✅ SHIPPED · ◑ PARTIAL · ○ AHEAD.

### Phase 0 — Stream inversion + schema (ships **P1**) — ✅ SHIPPED (`15a838e`)
- v72 migration (`auth_db_migrations_v72.c`, all columns + `conversation_events` table created-but-unused); the wire contract froze with `conversation_id` on deltas + the `status`/`complete`/`message_appended` additions reserved. `conv_stream.c` conversation-keyed ring; client delta router + sidebar generating indicator (reduced-motion fallback). **Live-verified** (§14).

### Phase 1 — Job pool + lifecycle + authz (ships **P2-minimal**) — ✅ SHIPPED (`2bbe683`)
- Separate job-session pool (`job_manager.c`: own `session_t[]` storage + `session_create_job`/`find_free_job_slot`/**cancel-then-wait teardown**, dual local/cloud counters); cancel/`disconnected` decoupling across all 5 sites incl compaction (P1 disconnect-survival); `job_tool.c` (`spawn`/`list`/`status`/`cancel`); `on_complete=notify` via coalesced scheduler delivery; bounded dirty-gated heartbeat monitor tick; boot interrupted-scan (no auto-reinvoke); extraction exemption; sidebar child filtering; v73 `deliver_to` column + monitor partial index. **§8 invariants 1,2,4,5** in place. TSan-gated.
- **Two deliberate simplifications vs. this plan (both re-scoped into the ledger, §14):** (a) **no queued *state machine*** — over-cap spawns *fail fast* rather than enqueue+promote (the row's momentary `queued` insert status is promoted straight to `running` by the worker), so `max_queued_per_user` + promotion are AHEAD; (b) **`spawn_depth` hardcoded to 1** — real depth propagation is the Phase-3 trees work.
- **`max_runtime_sec` reap** — shipped as follow-up commit `1ae36d9` (§5 "Runtime reap — as-built"); it was missing from the original Phase-1 commit despite being claimed in this doc.

### Phase 1.5 — Per-session turn queue + headless workers — ✅ SHIPPED (`2bbe683`), **design addition** (not in the original plan)
- **Turn queue** (§4a): serializes text/voice/reinvoke turns per session so two never race one `session_t`'s streaming state; teardown safety (`being_destroyed`/`closing`/purge-before-ref-wait), `session_destroy` leak-don't-free, `last_activity` `_Atomic`.
- **Headless job workers:** the `job` tool is masked out of a `SESSION_TYPE_JOB` session's schema + a headless-agent directive rides its stable prefix, so a worker does its task **inline** and reports — instead of behaving like interactive Friday (deferring / recursively spawning more jobs). Hard backstop refuses `spawn` from a job context.
- **Extraction exemption** landed at the `memory_recovery` scan (`AND job_status IS NULL`) — memory comes from the parent, not per-job research fragments.

### Phase 2 — Durable event log + observe surface — ◑ IN PROGRESS (CP1 + CP2 shipped, uncommitted)

**CP1 (event writers) and CP2 (attach/replay + line-printer gate) are BUILT and live-verified.**
Plan: `~/.claude/plans/immutable-shimmying-lark.md`. Remaining: CP3 WS control surface, CP4 jobs
panel, CP5 live-watch.

**As-built — amendments to §6/§8 that this phase actually shipped:**

- **§6.2 `status` is SCOPED, not universal.** Emitted only for job conversations, background/reinvoke
  turns, and turns whose conversation is a job (`job_status IS NOT NULL`) — three terms, because none
  alone covers a user typing into a job conversation from their own WebUI session. Interactive
  conversations are already observable via the sidebar chip, and universal emission roughly doubled
  the event table for no added visibility. **`tool_call`/`tool_result` are NOT scoped** — they fire
  for every conversation, because unlike a status heartbeat they are real content, and capturing them
  everywhere lets the event log eventually back the existing debug transcript.
- **§8.6 correction: there is NO reusable "log-redaction machinery" to pass payloads through.**
  `ENV_SECRET` is a config-parse logging macro. `src/core/event_payload.c` builds the redactor, and it
  is a **denylist + capability backstop**, deliberately not the allowlist §8.6 implies: an allowlist
  would render most args `<redacted>` and gut the observe surface this phase exists to build. A full
  registry audit found exactly one credential parameter (`shutdown_tool`'s `passphrase`), so three
  rules cover it — key-name pattern, value shape (`sk-`, `Bearer`, long opaque runs), and every arg of
  a `TOOL_CAP_SECRETS` tool. Rule 3 is the fails-safe an allowlist was wanted for, without the UX
  cost. **Live-verified**: 9 real search calls persisted fully readable.
- **§8.8 retention is KIND-AWARE.** `tool_call`/`tool_result`/`terminal_chunk` payloads are NULLed with
  the row kept (seq chain stays coherent, step renders as "expired"); `status` rows are DELETED
  outright (payload *is* their content; seq gaps are harmless since reads are `seq > last_seq`);
  `spawn`/`complete` kept intact. `[jobs] event_retention_days = 30`, **ON by default** — unlike the
  documents-retention precedent, this guards transient tool output, not authored content.
- **§6.4 `job_action` will ship `{cancel|resume}`; `spawn` is deliberately dropped** (creation is
  conversational per §7).
- **§6 accepted deviation:** deltas carry no byte offsets, so a delta enqueued between the attach's
  partial-dup and its snapshot can be lost. Self-healing — `message_appended` delivers the complete
  final text — so offsets were not built. Recorded rather than fixed.

**Key implementation seams (so a later reader doesn't re-derive them):**

- `conv_event_emit()` (`src/core/conv_event.c`) pairs **persist + fan-out in one call**. Splitting
  that across emit sites is how one of them eventually persists without broadcasting. Ordering is
  durable-first, which was validated by accident: a crash mid-fan-out left the `spawn` row intact.
- `complete` is emitted by `job_manager_set_terminal()` (Layer-2 wrapper), NOT inside
  `conv_db_job_set_terminal()` — the DB layer must not call a WebUI weak symbol. A job reaches a
  terminal state from **8** call sites including the boot interrupted-scan; the wrapper makes a 9th
  impossible to forget.
- `status` is emitted at entry/exit of `core_text_input_dispatch()`, which is synchronous and
  single-return, so one pair covers success, cancel and failure. **PTT audio is NOT on that path**
  (`webui_audio.c` calls `session_llm_call_*` directly) — accepted gap: PTT-into-a-job-conversation.
- `tool_call`/`tool_result` are emitted at the `llm_tool_loop.c` persist fire site, **before** its
  no-callback early return, so events aren't coupled to whether a conv-persist hook is installed.
- Attach ordering (messages → events → ring → live) is enforced inside `handle_load_conversation`;
  `attach_conversation` shares that handler and is distinguished only by carrying `last_seq`.

**Live-verified (real job, conv 1006):** 21 events, correct order, both tool-loop iterations, per-
conversation seq, `complete{done, final_message_id:21547}` correlating to the real answer row, and the
Python line-printer reconstructing the whole job from the wire alone — the Phase-5 TUI de-risk.
**Still unverified: the LIVE tail** (`conversation_event` frames arriving during a run); replay is
proven, live push is not.

**A double free found only by live testing:** `broadcast_json_to_user()` takes ownership of the JSON
tree; adding a `json_object_put()` after it corrupted the heap. Unit tests link the **weak no-op**
broadcast symbol, so no test could have caught it. Any future weak/strong broadcast seam needs a live
run or ASan.

### Phase 2 — Durable event log + observe surface (ships the contract) — ○ AHEAD (mandatory; the whole *observe* half)
`agent ~3-4d · api $0 · 5 ckpt` — **nothing here is built yet** beyond the activity pill; the `conversation_events` table sits empty.
- `conversation_events` **writers + seq**; **ownership-JOINed** reads (**§8.3**); secret redaction (**§8.6**) + render sanitization (**§8.7**) + retention/pruning (**§8.8**); full `attach_conversation {last_seq}` replay; **WS `list_jobs` / `job_action {spawn|cancel|resume}`** handlers (ownership-checked); **always-visible `.agent-event.*` rendering** (not debug-gated) distinguishing tool_call/tool_result/terminal/spawn/complete; **jobs panel as an indented tree** (by `spawn_depth`) with status/elapsed/cancel + a **global "agents: N running" count badge**; **live-watch = phone-panel twin** (server-authoritative anchor-elapsed, minimize-to-pill, reconnect rehydrate via `jobs_snapshot`, sr-only polite milestone announcer, `DawnEscStack`); toast target branches `notify`→job vs `reinvoke_parent`→parent. *(Shipped so far: only the amber activity **pill** — `jobs-activity.js` + a `jobs_activity_snapshot` frame. The panel/tree/live-watch and the whole durable-event contract are this phase.)*
- **Verify:** a **~100-line Python line-printer** speaking the handshake tails a live job from another machine and shows tool steps + the final answer (`message_appended`) + disposition — the TUI de-risk; panel survives reconnect with correct replay.

### Phase 3 — reinvoke_parent + trees (ships **P3**) — ◑ PARTIAL
`agent ~2-3d · api ~$2 · 4 ckpt` (remaining = trees only)
- ✅ **`reinvoke_parent` SHIPPED** (`2bbe683`) — but **re-architected through the §4a turn queue** (enqueue a background turn onto the live viewer's queue; envelope built at dequeue; exactly-once release; C3 persist-before-mark-fired), not the plan's direct DB-inject-and-redispatch. `max_reinvokes_per_tree` livelock guard, `idx_conversations_parent` derived join, dispatch-only-when-parent-idle, messaging/voice-parent → `notify` downgrade all landed.
- ○ **trees AHEAD** — multi-level spawn (`spawn_depth = parent+1`), **enforce** `max_spawn_depth`/`max_children_per_tree`, ownership-bounded **cascade cancel** + spawn-into-cancelling-tree guard + parent-delete reap, per-conversation settings inheritance, `awaited` UI sugar, **SAGE notification coalescing/budget** for many-job bursts. **Sequencing note:** headless workers currently *block* job→job spawning outright; re-enabling trees means lifting that block **only** behind enforced depth/children caps (correctness cap before the feature — so a bug can't fan out unbounded like the pre-headless runaway did).
- **Verify:** parent spawns 2 children → both complete → parent resumes once each (idempotent under induced crash between child-done and reinvoke); cancel parent kills children; depth-4 / timeout-refire-loop refused.

### Phase 4 — Workspace activation — ○ AHEAD (GATED on the operator-launched sandbox existing)
`agent ~2d · api $0 · 3 ckpt`
- `workspace_ref` semantics (MCP server + workspace id + **generation nonce**); inheritance policy on spawn (share **within own tree only** / fresh / worktree — **§8.9**); boot reattach with nonce validation; sandbox-count as a second cap dimension; `terminal_chunk` events into the Phase-2 log with sanitization + redaction. **Zero schema/contract change** — the payoff of baking the column + event kind in early. *(`workspace_ref` column reserved in v72; zero semantics today.)*

### Phase 5 — TUI client — ○ AHEAD (STRETCH)
Separate program consuming the Phase-2 contract; nothing daemon-side. Blocked on the Phase-2 event contract existing.

---

## 11. Conventions honored

- Return codes `SUCCESS`/`FAILURE` (positive-only); event/insert helpers use `int64_t *id_out`.
- GPL header on new C files; kept under the 1,500 soft limit (`conv_stream.c`, `job_tool.c`, and a `job_manager`/job-pool sibling rather than growing `session_manager.c`). v72 body in its own migration file; only the ~15-line ladder block touches the size-exempt `auth_db_migrations.c`.
- Layering: `conv_stream.c` L1 leaf (passive; fan-out driven from L4/bridge); job-pool/`job_manager` L2; `job_tool.c` L3; WebUI L4. Downward-only; monitor tick + live fan-out use the registered-fn/weak-symbol bridge, no upward dep.
- Settings in `dawn.toml` + `SETTINGS_SCHEMA`; job control conversational (tool + WebUI).
- WS send-funnel invariant preserved; no-ES-modules WebUI conventions (`window.DawnJobs` IIFE; compose `.dawn-badge` + `.dawn-status-dot` state tints per the `code-projects.css` precedent — no new chip component; `DawnDialog`/`DawnEscStack`/`DawnStore`/`DawnToast`; phone-panel reuse for live-watch).

---

## 12. Open items / deferred

- `conversation_waits` explicit join table; multi-child *ordered* joins — deferred (derive first).
- Voice/Session-0 turns do not background (they may *spawn* jobs); scoped out.
- Token-durable logging — rejected (step granularity only).
- True blocking `awaited` — rejected under slot scarcity; re-addable only after a generation-context refactor.
- `seq` MAX+1 → in-memory counter — deferred optimization if contention shows.
- Mixed-provider concurrency accounting edge cases — v1 uses two counters by resolved provider class; refine if telemetry shows contention.

---

## 13. Decisions (locked 2026-07-22)

1. Collapse `awaited` → `detached + reinvoke_parent` (UI sugar only). ✅
2. Separate `max_concurrent_local` (default 1) / `max_concurrent_cloud` (default 4) counters. ✅
3. Child conversations: nested expander in the sidebar; jobs panel renders the full tree; **click a running agent → watch output in real-time** (live-tail). ✅
4. Restart-resume: notification + explicit Resume button (no auto-resume; boot never auto-reinvokes a parent). ✅
5. Job conversations do not feed memory extraction; results feed back into the parent conversation. ✅
6. Column name: `parent_id`. ✅
7. **Sub-agents get a separate job-session pool (LLM-resource-bound), NOT the audio-bound `MAX_SESSIONS` interactive array.** ✅ *(this revision)*

### Security decisions (locked — §8)
8. Single-user tree; child inherits spawner `user_id`, non-overridable.
9. All new WS handlers ownership-check `conn->auth_user_id`; all event reads ownership-JOINed.
10. Global + per-user job caps + bounded queue + bounded per-tick monitor work.
11. Injection filter on spawn/cancel; a child controls only its own subtree.
12. Secret redaction before persisting event payloads; untrusted-text rendering for tool/terminal output.

---

## 14. Implementation Status

**On `main` (PR #23) — ⚠ NOT merged to `main`** (`main` = `b7e2bb8`; branch is 5 ahead):
Phase 0 = `15a838e`; Phase 1 + turn queue + reinvoke + headless + extraction exemption =
**`2bbe683`** (`feat(jobs,core): background jobs + per-session turn queue + reinvoke`); observe indicators =
`f42fce1`. All committed **on the branch** — a merge/PR to `main` is still outstanding.
(`98bc048` phone-broadcaster single-owner prerequisite preceded them, also branch-only.) A one-time **memory cleanup** ran
alongside the `2bbe683` work (not part of the commit — see the "Memory cleanup" note at the end of this section).

### Phase 0 — SHIPPED + live-verified (P1: background conversation generation)

The headline P1 outcome works end-to-end and was live-verified in the browser: **you can start a turn,
switch to another conversation, and its output does not bleed into the view you switched to; the full
conversation (tool iterations + final answer) is saved to the *right* conversation; switching back mid-
stream resumes it; and a turn that completes entirely in the background is persisted so it's there on
return.** The sidebar shows a steady pulsing dot on the generating conversation, and the status pill
reflects the conversation you're viewing (not a background turn).

**Schema (v72):** `src/auth/auth_db_migrations_v72.c` + base-schema columns + `conversation_events`
table (schema 71→72). 11 job columns on `conversations`, `parent_id` self-FK `ON DELETE SET NULL`,
`idx_conversations_parent`, UNIQUE `idx_conv_events`. Tests: `test_auth_db_migrations_v72` (4) + scratch-DB SQL.

**`conv_stream` (Layer-1 leaf):** `src/core/conv_stream.{c,h}` — conversation-keyed live-partial replay
ring (32-slot LRU, 64 KB/turn cap, leaf mutex, abandoned-active evict backstop). Tests: `test_conv_stream` (13).

**Conversation binding (never 0, robust to switch/reconnect/multi-tab):**
- The turn's conversation is **captured at dispatch** (`session->stream_conversation_id`, `_Atomic`), before
  any thinking/stream frame — closing the thinking-latency mis-tag window (the old M1).
- **Fresh-chat race fixed:** the client pre-creates the conversation *before* sending the first message
  (`DawnHistory.beginConversationBeforeSend`, ordered ahead of the text on the wire), and the server
  **back-fills** `stream_conversation_id` when it creates the row (`handle_new_conversation`) as a safety net.
  Together these eliminate the `conv=0` tagging that broke routing + persistence.
- **Explicit `conversation_id` per message (reviewer Fix 2):** `sendTextMessage` sends the active conversation
  id; the dispatcher (`webui_message_dispatch.c`) **validates ownership** (`conv_db_get` against the user) and
  heals `active_conversation_id` + the privacy flag from it — so the server is authoritative instead of
  inferring from a stale live view after reconnect/multi-tab.

**Frame tagging + routing:** `stream_start/delta/end`, `thinking_start/delta/end`, and `state` frames all
carry `conversation_id` (additive JSON — non-WebUI clients ignore it). Client `isBackgroundStream()` routes
purely by conversation: a background turn's tokens/thinking/state never touch the active view. `state`-pill
generating states are gated to the active conversation, and the pill resets to idle on conversation switch
(`resetSilently`); a switch-back to a still-streaming conversation re-raises it via `stream_resume`.

**Persistence, all keyed to the turn's captured conversation (not the live view):**
- Tool-iteration rows: `webui_tool_persist_cb` → captured conv (no live-view fallback).
- User message + focus injection: `dispatch_opts.conversation_id` = captured conv.
- **Backgrounded final answer (closes the old C1):** if a turn finishes while the client is viewing a
  *different* conversation, the worker persists the final answer server-side to the captured conversation
  (`webui_text_processing.c`, gated `turn_conv != on-screen conv` so a foreground turn — still client-saved —
  isn't double-written). Log line: `WebUI: persisted backgrounded assistant answer to conv N`.

**Client indicators:** `handleStreamResume` rebuilds + resumes a partial on switch-back; **silent**
streaming-state reset on switch (no stale partial persisted into the wrong conversation); sidebar generating
dot **modeled in `historyState`** (survives re-renders), lit across tool-loop iterations, absolutely
positioned (no title shift), reduced-motion fallback.

**Verification:** all changes build clean, 0 warnings, 104/104 CI, format clean; live-verified in the browser
across fresh chat, switch-away-mid-turn, switch-back-mid-stream, and switch-away-until-complete.

### Phase 1 — SHIPPED (`2bbe683`): job pool + lifecycle + tool + authz

- **Separate job-session pool** (`src/core/job_manager.c`, Layer 2): `session_manager_alloc_bare`/`_free_bare` over a
  cap-sized `session_t*` array (`SESSION_TYPE_JOB`); dual `n_running_local`/`n_running_cloud` counters; **cancel-then-wait
  teardown** (set `cancel_requested`, `pthread_cond_wait` on `ref_zero_cond`, then `free_bare` — no 3s proceed-anyway) with
  a bounded leak-don't-free backstop on a wedged worker. `session_manager_register_job_lookup` (fn-pointer, invoked only
  after releasing `session_manager_rwlock` — no lock inversion).
- **Worker + dispatch** (`job_worker.c`, `job_dispatch.c`): a detached worker runs the no-audio LLM tool loop on a job
  session via the shared `core_text_input_dispatch`; persists tool rows + final answer through `conv_db_*` (Layer-2-only,
  no WebUI callback). Boot interrupted-scan marks stale `running`/`queued` → `interrupted` (never auto-reinvoke across a
  restart).
- **`job_tool.c`** (Layer 3): `spawn`/`list`/`status`/`cancel`; `user_id` non-overridable (from session context);
  `memory_filter_check` on goals; budget gates (global/per-user/per-provider — **refuse past cap**); messaging/rootless
  parent → `notify` downgrade.
- **Monitor** (`jobs_monitor_tick`, main-loop 1-Hz, **dirty-gated** — zero idle DB work): drains ≤`monitor_followups_per_tick`
  terminal rows, coalesces completions into one alert, delivers off-thread (transient detached thread — blocking TTS/curl
  never on the heartbeat). v73 added the `deliver_to` column + partial index `idx_conv_job_followup`.
  **`max_runtime_sec` reap** (`job_manager_reap_overdue`, ahead of the dirty-gate, in-memory pool walk) —
  landed as follow-up commit `1ae36d9`, NOT in `2bbe683`; see §5 "Runtime reap — as-built".
- **Config** `[jobs]` (parser/defaults/`dawn_config.h` + `SETTINGS_SCHEMA`). **§8 invariants 1, 2 (tool), 4, 5** in place.
- Deliberate simplifications vs §10 Phase 1: **no queued *state machine*** (fail-fast past cap; the insert-time `queued`
  status is promoted immediately by the worker), **`spawn_depth` hardcoded to 1**, and **no `max_runtime_sec` reap**
  — all three tracked in the ledger below.

### Turn queue + reinvoke + headless + extraction — SHIPPED (`2bbe683`)

The concurrency foundation and the reinvoke re-architecture (full detail in **§4a**): the per-session **turn queue**
(`turn_queue.c`) with `being_destroyed`/`closing`/leak-don't-free teardown and `last_activity` `_Atomic`; **`reinvoke_parent`
via the queue** (marker enqueue, envelope-at-dequeue, exactly-once release, C3 persist-before-mark-fired,
`webui_find_reinvoke_viewer`/`webui_session_active_conversation` weak seams); **headless job workers** (`job` tool masked
from the job-session schema + `JOB_HEADLESS_DIRECTIVE` in the stable prefix + `handle_spawn` backstop) so a worker does its
task inline instead of fanning out; **extraction exemption** at the `memory_recovery` scan. Reviewed across P1/P2/headless
by architecture/security/embedded passes, then a **6-agent full pre-commit review** (all Critical/High/Medium fixed).

### Phase-0 residuals — reconciled

- ✅ **Zero-save race (Arch-H1):** RESOLVED — the text path is now server-authoritative (persist on `disconnected`/backgrounded
  at turn end), removing the two-racing-reads discriminator.
- ✅ **Phase-1 monitor index:** RESOLVED — v73 `idx_conv_job_followup` shipped.
- ◑ **Voice/audio backgrounded persist:** the audio producer was routed through the turn queue this cycle (`turn_in_flight`
  parity added), but the *backgrounded-voice-answer server-persist* was NOT mirrored from the text path — still open.
- ○ **Context-injection ("context for this turn") panel** still reads the live conversation for background turns (content
  panel only, not transcript/persistence).
  ✅ **Sidebar done/unread dot** + **per-conversation running-jobs pill** + generating-dot `aria-busy`/SR live-status
  SHIPPED (`f42fce1`) — dot marks a conversation that gained external content (background turn/job finishing, or a
  channel message) while viewed elsewhere; pill shows the authoritative running-job count per conversation.
  ✅ **`metrics_update` now conversation-gated** (`f42fce1`) — a background turn's tok/s/TTFT no longer updates the
  active view's footer (`conversation_id` threaded into the frame; client `isForeignConvFrame` gate).
  ○ **`stream_resume` vs async load-render ordering** (self-corrects on reload). — remaining deferred UI polish.

### Designed-but-not-yet-shipped ledger (the single checklist so no scope is lost)

- **Phase 2 — the entire *observe* half.** `conversation_events` table exists but **has zero writers/reads**: no event
  emission (`status`/`tool_call`/`tool_result`/`spawn`/`complete`), no `attach_conversation {last_seq}` replay, no WS
  `list_jobs`/`job_action` handlers, no jobs tree panel / global count badge / live-watch panel, no secret redaction
  (§8.6) / render sanitization (§8.7) / retention (§8.8), no TUI line-printer. *Shipped from the observe side (`f42fce1`):
  the global/composer activity pill, the sidebar per-conversation running-jobs pill + done/unread dot, and active-view
  `metrics_update` gating — the ambient indicators, not the panel/tree/live-watch.*
- **Queued state (Phase-1 simplification).** A queued row is *inserted* as `job_status='queued'` and cancel handles that
  status, but nothing ever waits in it: no monitor promotion, no `max_queued_per_user` enforcement — spawns fail fast past
  the running cap.
- ~~**`max_runtime_sec` reap**~~ — **DONE 2026-07-24, commit `1ae36d9`** (follow-up to `2bbe683`; see §5 "Runtime reap —
  as-built"). Closed the slot-leak: a wedged worker used to hold its pool slot + provider counter until daemon
  restart, so with the default `max_concurrent_local=1` one hung local job blocked every later local spawn.
  7 Unity cases in `tests/test_job_manager.c` (overdue, boundary, disabled-at-0, backwards-clock, reap-vs-user-cancel,
  all-overdue-in-one-pass, NULL/unknown session).
- **Trees (Phase 3).** `spawn_depth=parent+1` propagation, `max_spawn_depth`/`max_children_per_tree` **enforcement**,
  cascade cancel, spawn-into-cancelling-tree guard, parent-delete reap, settings inheritance, `awaited` UI sugar. *(Headless
  workers currently block job→job spawning; re-enable only behind enforced caps.)*
- **Resume.** Boot marks `interrupted` + notifies ✅, but the explicit **Resume button** / `job_action{resume}` (re-dispatch
  with history + "you were interrupted" line) is not built.
- **Phase 4 — workspace/sandbox.** `workspace_ref` reserved; zero semantics (MCP reattach, generation nonce, inheritance,
  `terminal_chunk` events, sandbox-count cap).
- **Phase 5 — TUI client.** Stretch; blocked on the Phase-2 event contract.
- **Unused `[jobs]` config knobs = shadows of the above (4, re-verified 2026-07-24):** `max_queued_per_user` (queued
  state), `max_spawn_depth` / `max_children_per_tree` (trees), `event_chunk_cap` (event-payload truncation) — parsed +
  clamped but with zero read sites outside `src/config/`.
  *Enforced today:* `max_active_jobs`, `max_jobs_per_user`, `max_concurrent_local`/`_cloud`,
  `monitor_followups_per_tick`, `max_reinvokes_per_tree`, `max_runtime_sec`.
- **Deferred code-review items** (from the 6-agent pass; tracked in the developer-local `docs/TODO.md`): `session_manager.c` split (over the 2500
  hard limit), shutdown worker-join barrier, live-reinvoke spawn asymmetry (the Phase-3 sequencing note), `client_data`
  TOCTOU one-int freed-read, lost-cancel race, Stop-doesn't-drain-queue, `sb_append`↔`strbuf`. Plus the **pre-existing**
  `local` (WEBUI-off) preset link break (`session_manager.c` sits in the ENABLE_WEBUI CMake block — orthogonal to this work).

### Memory cleanup (one-time data op, 2026-07-24 — NOT part of the commit)

Before the extraction exemption shipped, every historical background job's conversation was being extracted into semantic
memory (job convs weren't exempt from the `memory_recovery` scan). That flooded the fact store with per-job research
fragments (325 facts / 484 relations / 33 summaries / 301 orphaned entities — ~9% of the user's facts). With a verified
`auth.db` backup (`/var/lib/dawn/auth.db.bak.jobmem_cleanup.20260724_064420`), those job-sourced rows + orphaned entities
were deleted (`memory_facts_fts` left as-is — contentless FTS the daemon already filters stale rowids from; `foreign_key_check`
clean; no `memory_*` FK violations). Going forward the exemption prevents recurrence — job research reaches memory only via
the parent conversation's own extraction.
