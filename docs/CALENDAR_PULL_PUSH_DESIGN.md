# Calendar Pull + Push (WebSocket) — Design

**Status:** in-flight (backend only; no `www/js` legacy-WebUI wiring yet — deferred by request).
**Origin:** `dawn-nextgen/docs/DAWN_UI_SIGNAL_MAP.md §9` item #4 (pull half; the *content push* half
here is the signal-only refetch, **not** the SAGE proactive-notice feed, which stays deferred).

## Goal

Give any front-end two things it cannot get today:

1. **Pull** — a request to read a window of upcoming calendar occurrences (build a "today / this week"
   panel). Backend already exists (`calendar_service_range`); this only adds the WS surface.
2. **Push** — a signal-only "the calendar data changed, refetch" broadcast so a panel stays live without
   polling. This is the idiomatic DAWN UI-sync signal (mirrors `scheduler_events_changed`), **not** a
   proactive ambient notice.

Explicitly **out of scope** (SAGE-tier, deferred): any content-bearing/proactive "your standup moved"
push — that rides the existing `silent_observation` / `attention_alert` channels and needs a judgment
layer. This design carries **no event PII on the push**.

## Non-goals

- No `www/js` client consumer (legacy WebUI integration deferred — discuss later).
- No email pull (separate, heavier — two backends, no offline cache).
- No mutation surface (create/update/delete already exist via the calendar tool; not adding WS mutation).

---

## Pull

### Request `calendar_upcoming_events`
Authenticated (per-user; each user sees only their own accounts' occurrences).

```json
{
  "type": "calendar_upcoming_events",
  "payload": {
    "days": 7,
    "calendar_name": "Work"
  }
}
```

Window resolution (in the handler):
- `{days}` — the **convenience path**: from `now` to `now + days*86400`. Default **7**, clamped **1..90**.
- OR explicit `{start, end}` epoch seconds — the **power path**: used when **both** are present and valid
  (`start < end`, span clamped to **≤ 366 days** — a wider ceiling than `days` on purpose, since an
  explicit window is a deliberate caller choice). **Exactly one of `start`/`end` present is a client bug →
  `success:false`** with an error string (not a silent fallback). An invalid explicit range (`start >= end`)
  is likewise `success:false`.
- `calendar_name` optional, case-insensitive; omitted/empty → all of the user's active calendars.
- Result capped at **`CAL_UPCOMING_MAX` = 256** occurrences (named constant, mirrors the existing
  `MAX_EVENTS` cap discipline in `calendar_tool.c`). Because the query orders by start ASC, the cap drops
  the **farthest** events; the response carries **`truncated: true`** when the cap is hit (jobs-panel
  truncation discipline) so a busy week never silently renders short.

### Handler `handle_calendar_upcoming_events(conn, payload)` — `webui_calendar.c`
Copies the established handler idiom in that file (`conn_require_auth` → build response object →
`success` + array → `send_json_response` → `json_object_put`).

```c
int count = calendar_service_range(conn->auth_user_id, start, end, cal_name, occ, CAL_UPCOMING_MAX);
```

`calendar_service_range` reads **pre-expanded occurrences straight from the offline SQLite cache** — no
network, no RRULE math at request time.

**All-day fix (shared, in the service function).** As written today, `calendar_service_range` calls only
`calendar_db_occurrences_in_range`, whose SQL hardcodes `AND o.all_day = 0` — so it returns **zero all-day
events** (holidays, birthdays, PTO). This is a pre-existing latent bug: the LLM tool's range path
(`calendar_tool.c:280`) drops all-day events too. Fix it **once, at the service function**, so both the pull
and the tool are correct: after the timed query, convert the `[start,end]` epoch window to local
`YYYY-MM-DD` strings (same `localtime_r`+`tm_gmtoff` approach `calendar_service_today` uses — `tz_name` is a
documented system-offset approximation there, no new param needed) and merge
`calendar_db_allday_occurrences_in_range`, then **`qsort` the merged array by `dtstart` ASC** (all-day rows
carry a `dtstart` epoch, so the ordering is well-defined; the raw merge would otherwise append all-day after
timed and break the start-ordering the cap relies on). `calendar_service_today` is left untouched (it does
its own today-scoped merge); only the range path changes.

### Response `calendar_upcoming_events_response`
```json
{
  "type": "calendar_upcoming_events_response",
  "payload": {
    "success": true,
    "start": 1784949199,
    "end": 1785554000,
    "events": [
      {
        "id": 412,
        "calendar_id": 7,
        "uid": "abc123@google.com",
        "summary": "Standup",
        "location": "",
        "start": 1784971800,
        "end": 1784973600,
        "all_day": false,
        "start_date": "",
        "end_date": "",
        "cancelled": false,
        "is_override": false
      }
    ],
    "truncated": false
  }
}
```
Serialized from `calendar_occurrence_t`: `id`, `calendar_id` (owning calendar — the grouping/coloring key;
map to name/color via `calendar_list_calendars`), `event_uid`→`uid`, `summary`, `location`,
`dtstart`→`start`, `dtend`→`end`, `all_day`, `dtstart_date`/`dtend_date`→`start_date`/`end_date` (only
meaningful for all-day), `is_cancelled`→`cancelled`, `is_override`. `start`/`end` echo the resolved window
so a client can label the panel without recomputing.

**Why `calendar_id`, not `calendar_name`.** A multi-calendar panel wants to group/color by source. The
occurrence's parent `calendar_events` row is already JOINed in both range queries, so `e.calendar_id` is a
free trailing column — no extra table join. The calendar *name* lives in the `calendars` table and would
force a second join, so it's deliberately left out; `calendar_id` is the stable grouping key anyway (names
change/collide). The id rides the shared `row_to_occurrence` mapper's existing variable-column pattern
(guarded on `sqlite3_column_count`), so only the two range statements select it; other occurrence queries
are unaffected (field stays 0).

**The id→{name,color} map: `calendar_list_my_calendars`.** So the client doesn't have to bootstrap the map
via `calendar_list_accounts` + a per-account `calendar_list_calendars` (an N+1), a companion request returns
the user's **active** calendars flat across accounts — `{id, account_id, name, color}` — in one call, backed
by the existing single-query `calendar_db_active_calendars_for_user`. Active-only is exact: it's the same set
the pull draws events from, so every `calendar_id` an event can carry is in the map. Re-run on
`calendar_events_changed` (a newly-synced calendar can appear).

`calendar_service_range` returns 0 on empty **or** error (same as the calendar tool sees), so the handler
reports `success:true` with a possibly-empty array. A distinct hard-error path isn't available from the
service without a signature change — out of scope; an empty window is indistinguishable from a read error,
same contract the LLM tool already lives with. **Panel implication:** a transient CalDAV read failure renders
as a legitimately-empty window rather than an error state. This is self-healing — the `calendar_events_changed`
push (below) fires after the next successful sync and re-drives the client's refetch, so the panel recovers
without user action. A client that wants to distinguish "empty" from "stale" can debounce on the push.

---

## Push

### Broadcast `calendar_events_changed` (signal-only)
```json
{ "type": "calendar_events_changed" }
```
Empty payload — a pure "refetch" nudge, identical in spirit to `scheduler_events_changed`. **No event data
on the wire**, so no PII, no per-frame redaction concern. Routed to the **owning user's** WebUI sessions
only (not satellites — no calendar panel there; matches the scheduler broadcast's WebUI-only routing).

### Emit point — inside `calendar_service_sync_now()`
`sync_now` already knows whether anything changed: it ctag-gates each calendar and increments a local
`synced` counter only when a calendar's ctag moved and its events were re-fetched (`calendar_service.c`
~L700). Emit once, at the tail, when `synced > 0`, keyed to `acct.user_id` (already loaded at the top of
the function). The call is **unconditional** (see the weak-symbol shape below — a definition exists in
every build):

```c
   OLOG_INFO("calendar: synced %d calendars for '%s'", synced, acct.name);
   if (synced > 0)
      calendar_broadcast_events_changed(acct.user_id);
   return 0;
```

This covers **all three** sync entry points for free — the background sync thread, the WS manual
`calendar_sync_account`, and the initial post-add sync (L299) — so a change made on one surface refreshes
every other surface. Firing is change-gated, so an idle account that syncs-and-finds-nothing emits nothing.

### Layering — weak symbol (Layer 3 → Layer 4), mirroring scheduler *exactly*
`calendar_service.c` is Layer 3; `webui/` is Layer 4. A direct call up-layer is forbidden. The sanctioned
pattern (copied from `scheduler_broadcast_events_changed`, `scheduler.c:108-111` + `scheduler.h`) is a
**compiled weak no-op definition**, *not* an `if(symbol)` NULL-guard — a bare weak declaration resolves to
address 0 and would segfault, so the guard form is wrong:

- **Prototype in `calendar_service.h`** (plain, non-weak) so the Layer-3 caller and the Layer-4 definer
  share one signature and can't drift:
  ```c
  void calendar_broadcast_events_changed(int user_id);
  ```
- **Weak no-op *definition* in `calendar_service.c`, compiled only when WebUI is OFF** — this is the key
  correction. `calendar_service.c` is gated on `DAWN_ENABLE_CALENDAR_TOOL`, not `ENABLE_WEBUI`, so it
  compiles in the `local`/`ci` (WEBUI-off) presets; the fallback definition is what keeps those builds
  linking and running:
  ```c
  #ifndef ENABLE_WEBUI
  void calendar_broadcast_events_changed(int user_id) __attribute__((weak));
  void calendar_broadcast_events_changed(int user_id) { (void)user_id; }
  #endif
  ```
- **Strong definition in `src/webui/webui_broadcasts.c`** (only compiled when WEBUI is on), right beside
  `scheduler_broadcast_events_changed` — builds the `calendar_events_changed` frame and fans out to the
  user's browser connections via the existing registry walk + `queue_response`/`send_json_response` funnel.
  When WEBUI is on, the `#ifndef` fallback is not compiled, so there's exactly one definition; when off,
  only the weak no-op exists. Unconditional call site in both cases — no duplicate symbol, no NULL deref.

Already called off-thread from the scheduler background thread today, so calling it from the calendar sync
thread and the lws service thread is the same proven pattern. No new thread, no new lock: the broadcast's
own connection-registry mutex is a leaf, and the calendar sync holds no lock across the emit (it's at the
tail, after `sodium_memzero`/`account_update_sync`).

---

## Files touched (backend only)

| File | Change |
|------|--------|
| `include/webui/webui_calendar.h` | prototypes `handle_calendar_list_my_calendars` + `handle_calendar_upcoming_events` |
| `src/webui/webui_calendar.c` | `handle_calendar_list_my_calendars` (flat id→{name,color} map, one call) |
| `include/tools/calendar_db.h` | `calendar_id` field on `calendar_occurrence_t` |
| `src/tools/calendar_db.c` | `row_to_occurrence` populates `calendar_id` (variable-column guard) |
| `src/auth/auth_db_statements.c` | `e.calendar_id` trailing column on both range statements |
| `src/webui/webui_calendar.c` | the pull handler + `CAL_UPCOMING_MAX`; window parse/clamp; occurrence serialize + `calendar_id` + `truncated` |
| `src/webui/webui_message_dispatch.c` | one dispatch arm `calendar_upcoming_events` |
| `include/tools/calendar_service.h` | prototype `calendar_broadcast_events_changed` |
| `src/tools/calendar_service.c` | (a) all-day merge + sort in `calendar_service_range` (shared fix); (b) `#ifndef ENABLE_WEBUI` weak no-op def; (c) change-gated unconditional emit in `sync_now` |
| `src/webui/webui_broadcasts.c` | strong `calendar_broadcast_events_changed` (copy scheduler sibling) |
| `docs/WEBSOCKET_PROTOCOL.md` | document request/response + broadcast |
| `dawn-nextgen/docs/DAWN_UI_SIGNAL_MAP.md` | mark #4 pull shipped; note the signal-only push |

## Security / correctness notes (for review)

- **Auth:** handler is `conn_require_auth`-gated; `calendar_service_range` is called with
  `conn->auth_user_id`, so cross-user reads are structurally impossible (the service filters by user).
- **Input validation:** `days` and `start/end` clamped/validated before use; `calendar_name` is passed to a
  parameterized query path (existing service), not concatenated.
- **PII:** pull returns the user's own event summaries/locations to that same authenticated user (fine).
  Push carries **no** event data — signal only — so a stray broadcast can never leak calendar content.
- **Push routing:** user-scoped, browsers-only; a satellite never receives it.
- **DoS shape:** result capped at 256; window span capped at 366 days; the read is a bounded indexed
  range query over the local cache. No unbounded work.
