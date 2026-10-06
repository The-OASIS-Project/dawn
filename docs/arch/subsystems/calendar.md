# CalDAV Calendar Subsystem

Source: `src/tools/caldav_client.c`, `src/tools/calendar_db.c`, `src/tools/calendar_service.c`, `src/tools/calendar_tool.c`

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
│  → RFC 4791 PROPFIND/REPORT/PUT/DELETE over HTTPS                    │
│  → Principal and calendar-home-set discovery                          │
│  → REPORT calendar-query with time-range filters                      │
│  → iCalendar (RFC 5545) parsing via libical                           │
└───────────────────────────────────────────────────────────────────────┘
```

## Key Design Points

- **Multi-account**: supports multiple CalDAV accounts simultaneously (e.g., personal Google + work Nextcloud).
- **Offline-first**: events are cached locally in SQLite; queries hit the DB, not the network.
- **RRULE expansion**: recurring events are pre-expanded into `event_occurrences` so range queries are simple SQL.
- **Background sync**: a dedicated thread periodically pulls changes from CalDAV servers using ctag/etag for efficiency.
- **Provider compatibility**: tested with Google Calendar, Apple iCloud, Nextcloud, and Radicale.
- **Calendars of the same name** (`calendar_pick.c`, pure): two of a user's calendars can share a display name (a "Home" in iCloud and in Google). The listing shows each with its account ("Home (iCloud)", or "Home (Google #12)" when one account has both). Adding an event to a name two writable calendars share is a question, never a guess; the label picks one. A read by display name covers every calendar of that name, and a read by label covers just that one.
