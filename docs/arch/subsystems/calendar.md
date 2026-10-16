# CalDAV Calendar Subsystem

Source: `src/tools/caldav_client.c`, `src/tools/calendar_ical.c`, `src/tools/calendar_rrule.c`, `src/tools/calendar_db.c`, `src/tools/calendar_service.c`, `src/tools/calendar_tool.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: CalDAV calendar integration — query, create, update, and delete calendar events across multiple providers (Google Calendar, iCloud, Nextcloud, Radicale) via the standard RFC 4791 protocol.

## Architecture: 4-Layer Design

```
┌───────────────────────────────────────────────────────────────────────┐
│                     LLM TOOL INTERFACE                                │
│  calendar_tool.c                                                      │
│  Actions: today | range | next | search | add | update | delete       │
│  → Registered via tool_registry, invoked by LLM tool loop             │
├───────────────────────────────────────────────────────────────────────┤
│                     BUSINESS LOGIC                                    │
│  calendar_service.c                                                   │
│  → Multi-account routing (personal, work, shared calendars)           │
│  → Background sync thread (configurable interval)                     │
│  → RRULE expansion via libical (pre-expanded occurrences in DB)       │
│  → Conflict detection and timezone normalization                      │
├───────────────────────────────────────────────────────────────────────┤
│                     SQLITE STORAGE                                    │
│  calendar_db.c                                                        │
│  Tables: caldav_accounts, calendars, events, event_occurrences        │
│  → Shares auth_db SQLite handle                                       │
│  → Pre-expanded occurrences for fast range queries                    │
│  → ctag/etag tracking for efficient sync                              │
├───────────────────────────────────────────────────────────────────────┤
│                     CALDAV PROTOCOL                                   │
│  caldav_client.c                                                      │
│  → RFC 4791 PROPFIND/REPORT/GET/PUT/DELETE over HTTPS                 │
│  → Principal and calendar-home-set discovery                          │
│  → REPORT calendar-query with time-range filters                      │
│  calendar_ical.c (no I/O)                                             │
│  → iCalendar (RFC 5545) reading, building and editing via libical     │
└───────────────────────────────────────────────────────────────────────┘
```

## Key Design Points

- **Multi-account**: supports multiple CalDAV accounts simultaneously (e.g., personal Google + work Nextcloud).
- **Offline-first**: events are cached locally in SQLite; queries hit the DB, not the network.
- **RRULE expansion**: recurring events are pre-expanded into `event_occurrences` so range queries are simple SQL. A series' cancelled dates (EXDATE) are left out and its moved instances (VEVENTs with a RECURRENCE-ID) are stored at their new times. Each instance carries its original start as `recurrence_id`, the key the tool shows as `occurrence:` and takes back to change or cancel that one instance.
- **Edits keep the event**: an update or delete reads the event fresh from the server (GET), changes only what was asked in the server's own iCalendar (`calendar_ical_edit`), and writes it back with `If-Match`, once more after a 412. Moving a series moves its cancelled and moved instances with it. An edit is refused, not attempted, when libical couldn't read part of the event, when the user is a guest on it (reminders only), when its zone isn't one libical has built in (times), or when a series moves to another weekday without its new rule. Writes are serialized per account.
- **Time zones are bounded**: a TZID is looked up only when it names a zone libical lists, UTC by any spelling, or an old name tzdata's own list (`tzdata.zi`) gives a listed current name (`calendar_rrule_zone`); otherwise it is read through a VTIMEZONE the calendar defines (only when its rules are bounded) or as UTC. libical therefore never opens a zone file by a name a server chose, so endless aliases can't grow the daemon.
- **Background sync**: a dedicated thread periodically pulls changes from CalDAV servers using ctag/etag for efficiency.
- **Provider compatibility**: tested with Google Calendar, Apple iCloud, Nextcloud, and Radicale.
- **Calendars of the same name** (`calendar_pick.c`, pure): two of a user's calendars can share a display name (a "Home" in iCloud and in Google). The listing shows each with its account ("Home (iCloud)", or "Home (Google #12)" when one account has both). Adding an event to a name two writable calendars share is a question, never a guess; the label picks one. A read by display name covers every calendar of that name, and a read by label covers just that one.
