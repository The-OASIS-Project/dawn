# Scheduler Subsystem

Source: `src/core/scheduler.c`, `src/core/scheduler_db.c`, `src/core/briefing_prompt.c`, `src/tools/scheduler_tool.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Timers, alarms, reminders, and scheduled tool execution with audible chimes and WebUI notifications.

## Architecture: Background Thread + SQLite + Tool Interface

```
┌───────────────────────────────────────────────────────────────────────┐
│                     LLM TOOL INTERFACE                                │
│  scheduler_tool.c                                                    │
│  Actions: create | list | cancel | query | snooze | dismiss          │
│  → "Set a 10 minute timer", "Remind me at 3pm", "Cancel all timers" │
├───────────────────────────────────────────────────────────────────────┤
│                     SCHEDULER ENGINE                                  │
│  scheduler.c                                                         │
│  → Background thread polls every second                              │
│  → Fires events when time arrives (chime audio + WebSocket notify)   │
│  → Recurrence: daily, weekdays, weekends, weekly, custom days        │
│  → Snooze (configurable duration) and dismiss                        │
│  → Scheduled tasks: execute any registered tool at a given time      │
├───────────────────────────────────────────────────────────────────────┤
│                     SQLITE STORAGE                                    │
│  scheduler_db.c                                                      │
│  Table: scheduler_events (user_id, type, label, fire_at, recurrence) │
│  → Shares auth_db SQLite handle                                      │
│  → Prepared statements for CRUD and time-range queries               │
└───────────────────────────────────────────────────────────────────────┘
```

## Key Design Points

- **Event types**: timer (countdown), alarm (absolute time), reminder (absolute + label), task (absolute + tool invocation), briefing (multi-step tool run + one LLM summarization at fire time).
- **Briefings**: a briefing runs its `briefing_steps` on a detached thread, concatenates the output, and makes one LLM summarization call under a `<briefing_data>` block. An optional per-briefing `instructions` field (schema v84, `sched_event_t.instructions`) steers *how* that summary is written (emphasis, structure, length, tone); it is emitted as a fenced `<briefing_instructions>` block that precedes an absolute security rule, so instructions can shape format but can never reclassify the data as commands. The field is LLM-editable in place via the scheduler `update` action (replace-wholesale semantics). Legacy briefings with no instructions use the fixed default prompt.
- **Chime audio**: built-in chime WAV played via audio subsystem at configurable volume.
- **WebUI notifications**: `scheduler_fire` WebSocket message triggers banner with snooze/dismiss buttons.
- **Recurrence**: events auto-reschedule after firing based on recurrence pattern.
- **94 unit test assertions** across 16 tests in `tests/test_scheduler.c`.
