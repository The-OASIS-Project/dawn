# Per-User Settings

Source: `src/auth/auth_db_settings.c`, `src/webui/webui_settings.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Per-user personalization — persona, location, timezone, units, theme.

## Key Components

- **auth_db_settings.c**: `user_settings` SQLite table with per-user preferences
   - Persona (append or replace mode), location, timezone, units, theme
   - CRUD via prepared statements on shared auth_db handle

- **webui_settings.c + my-settings.js**: "My Settings" WebUI panel
   - Per-user preferences editable from the browser
   - Theme selection syncs immediately

- **build_user_prompt()**: System prompt personalization
   - Injects user preferences (persona, location, units) into LLM system prompt at session start
   - Each session is personalized to the authenticated user
- **user_tz()** (`src/core/user_tz.c`): the user's time zone, else the server's
   - Named in each turn's `[system_time]` line; the date, time, calendar and scheduler tools read and
     write clock times in it
