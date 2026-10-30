# Per-User Settings

Source: `src/auth/auth_db_settings.c`, `src/webui/webui_settings.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Per-user personalization — persona, location, timezone, units, theme.

## Key Components

- **auth_db_settings.c**: `user_settings` SQLite table with per-user preferences
   - Persona in two halves, values and voice (v112), each with its own text and mode: added to the
     server's half, or replacing it
   - The older single persona text (`persona_description`, `persona_mode`) is kept: a replace-mode
     one still replaces the whole persona, an append-mode one is still added to the user's context,
     until the user clears it
   - Location, timezone, units, theme
   - CRUD via prepared statements on shared auth_db handle

- **webui_settings.c + my-settings.js**: "My Settings" WebUI panel
   - Per-user preferences editable from the browser
   - Theme selection syncs immediately

- **build_user_prompt()**: System prompt personalization
   - Injects user preferences (persona, location, units) into LLM system prompt at session start
   - Each session is personalized to the authenticated user
   - The persona composes the same way at both levels: each half is the server's (`[persona] values`
     / `voice`, else the built-in; while the older `[persona] description` is set it is the values
     half and the voice half is empty), then the user's text for that half is added after a blank
     line or replaces it; the halves join with a blank line (`llm_persona_half`,
     `prompt_persona_half`, `prompt_persona_join`)
   - Preferred address is a comma-separated choice the model uses now and then
- **user_tz()** (`src/core/user_tz.c`): the user's time zone, else the server's
   - Named in each turn's `[system_time]` line; the date, time, calendar and scheduler tools read and
     write clock times in it
