/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Scheduler Database Layer - CRUD operations for scheduled_events table
 *
 * Provides all SQLite operations for the scheduler. Uses the shared auth_db
 * database handle and prepared statements. All functions are thread-safe
 * via the auth_db mutex.
 */

#ifndef SCHEDULER_DB_H
#define SCHEDULER_DB_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "dawn_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

/* Scheduler DB return codes */
#define SCHED_DB_SUCCESS SUCCESS
#define SCHED_DB_FAILURE FAILURE
#define SCHED_DB_USER_LIMIT 2
#define SCHED_DB_GLOBAL_LIMIT 3
/* Row exists but cannot be edited: not owned by the caller, not a briefing, or
 * no longer in an editable status (already fired/cancelled/ringing). Distinct
 * from SCHED_DB_FAILURE (a real DB error) so the tool can report the difference. */
#define SCHED_DB_NOT_EDITABLE 4

#define SCHED_NAME_MAX 128
#define SCHED_MESSAGE_MAX 512
#define SCHED_UUID_MAX 37
#define SCHED_LOCATION_MAX 32
#define SCHED_TOOL_NAME_MAX 64
#define SCHED_TOOL_VALUE_MAX 2048
#define SCHED_RECURRENCE_DAYS_MAX 32
#define SCHED_ORIGINAL_TIME_MAX 6 /* HH:MM + null */
#define SCHED_MAX_RESULTS 50
/* Messaging channel display_name cap for fan-out delivery (schema v54).
 * Sized to the messaging engine's channel name surface — display names are
 * user-supplied via /link but the engine caps inbound, so 64 is comfortable
 * margin.  Empty = no fan-out (legacy default). */
#define SCHED_DELIVER_TO_MAX 64

/* Hard cap on briefing step count.  Not a config knob — at 9+ steps the LLM
 * summarization context approaches diminishing returns and the briefing
 * duration becomes user-hostile. */
#define SCHED_BRIEFING_STEPS_MAX 8

/* Tripwire: list / batched-lister call sites stack-allocate an
 * `int64_t ids[SCHED_MAX_RESULTS]` array (400 bytes at 50) and
 * `sched_event_t active[SCHED_MAX_RESULTS]` / `missed[]` (~3 KB each at 50,
 * so ~294 KB total for both arrays at the WebUI panel path).  If
 * SCHED_MAX_RESULTS bumps past this ceiling, those call sites need to move
 * to heap allocation alongside the step table they already do.  Pinning
 * here so the ceiling can't drift without a build-break review.  See
 * src/tools/scheduler_tool.c::handle_list and
 * src/webui/webui_scheduler.c::handle_scheduler_list_events. */
_Static_assert(SCHED_MAX_RESULTS <= 256,
               "Bumping SCHED_MAX_RESULTS past 256 requires moving the ids[] + active[]/missed[] "
               "stack arrays in handle_list / handle_scheduler_list_events to heap.");

/* =============================================================================
 * Enums (C enums, convert at DB boundary)
 * ============================================================================= */

typedef enum {
   SCHED_EVENT_TIMER,
   SCHED_EVENT_ALARM,
   SCHED_EVENT_REMINDER,
   SCHED_EVENT_TASK,
   SCHED_EVENT_BRIEFING,
} sched_event_type_t;

typedef enum {
   SCHED_STATUS_PENDING,
   SCHED_STATUS_RINGING,
   SCHED_STATUS_FIRED,
   SCHED_STATUS_CANCELLED,
   SCHED_STATUS_SNOOZED,
   SCHED_STATUS_MISSED,
   SCHED_STATUS_DISMISSED,
   SCHED_STATUS_TIMED_OUT,
} sched_status_t;

typedef enum {
   SCHED_RECUR_ONCE,
   SCHED_RECUR_DAILY,
   SCHED_RECUR_WEEKDAYS,
   SCHED_RECUR_WEEKENDS,
   SCHED_RECUR_WEEKLY,
   SCHED_RECUR_CUSTOM,
} sched_recurrence_t;

typedef enum {
   SCHED_SOURCE_LOCAL = 0, /**< Local mic — daemon speaker (default/legacy) */
   SCHED_SOURCE_WEBUI = 1, /**< WebUI browser session */
   SCHED_SOURCE_DAP2 = 2,  /**< DAP2 satellite */
} sched_source_type_t;

/* Per-briefing TTS override.  Tri-state because a missing field needs to fall
 * back to the source heuristic, NOT to a hard yes/no.  Stored in
 * scheduled_events.say_aloud as the underlying int (schema v53). */
typedef enum {
   SCHED_SAY_ALOUD_DEFAULT = 0, /**< Use source heuristic (voice=speak, webui=config-gated) */
   SCHED_SAY_ALOUD_ALWAYS = 1,  /**< Force TTS regardless of source */
   SCHED_SAY_ALOUD_NEVER = 2,   /**< Suppress TTS regardless of source */
} sched_say_aloud_t;

/* =============================================================================
 * Event Structure
 * ============================================================================= */

typedef struct {
   int64_t id;
   int user_id;
   sched_event_type_t event_type;
   sched_status_t status;
   char name[SCHED_NAME_MAX];
   char message[SCHED_MESSAGE_MAX];
   time_t fire_at;
   time_t created_at;
   int duration_sec;
   time_t snoozed_until;
   sched_recurrence_t recurrence;
   char recurrence_days[SCHED_RECURRENCE_DAYS_MAX];
   char original_time[SCHED_ORIGINAL_TIME_MAX];
   char source_uuid[SCHED_UUID_MAX];
   char source_location[SCHED_LOCATION_MAX];
   sched_source_type_t source_client_type;
   bool announce_all;
   /* Legacy single-tool fields — populated for tasks AND for pre-v50
    * backfilled briefings.  Post-v50 NEW briefings write to briefing_steps
    * instead; these fields stay empty on new briefing rows.  Reads must
    * check briefing_steps first and fall through to these only when the
    * steps list is empty (legacy backfilled row). */
   char tool_name[SCHED_TOOL_NAME_MAX];
   char tool_action[SCHED_TOOL_NAME_MAX];
   char tool_value[SCHED_TOOL_VALUE_MAX];
   time_t fired_at;
   int snooze_count;
   /* Per-briefing TTS override (schema v53).  Tri-state — DEFAULT falls back
    * to source heuristic + the [scheduler] briefing_speak_aloud_on_webui_source
    * config flag.  Only meaningful for SCHED_EVENT_BRIEFING; harmless on
    * other types (read but ignored at fire time).  Default 0 preserves
    * pre-v53 behavior on existing rows after migration. */
   sched_say_aloud_t say_aloud;
   /* Messaging channel for fan-out delivery (schema v54).  Empty = no
    * fan-out (legacy default; preserves pre-v54 behavior on existing
    * rows).  When non-empty, the scheduler fires the announcement /
    * briefing through scheduler_send_to_messaging_channel after the
    * existing TTS + WebUI banner path.  Ownership check is at the
    * engine layer (messaging_engine_send verifies the channel belongs
    * to user_id) — prevents a stale row from leaking briefings into
    * another user's channel.  Meaningful for ALL event types — the
    * fan-out fires in announce_event (timer/alarm/reminder/task) and
    * in briefing_thread_func (briefing success + failure).  Alarm
    * auto-dismiss + timeout terminal-state notifications do NOT
    * re-fan-out (the initial ring already did). */
   char deliver_to[SCHED_DELIVER_TO_MAX];
} sched_event_t;

/* Single step within a multi-step briefing.  Schema v50+. */
typedef struct {
   char tool_name[SCHED_TOOL_NAME_MAX];
   char tool_action[SCHED_TOOL_NAME_MAX];
   char tool_value[SCHED_TOOL_VALUE_MAX];
} sched_briefing_step_t;

/* =============================================================================
 * String Conversion Helpers
 * ============================================================================= */

const char *sched_event_type_to_str(sched_event_type_t type);
sched_event_type_t sched_event_type_from_str(const char *str);
const char *sched_status_to_str(sched_status_t status);
sched_status_t sched_status_from_str(const char *str);
const char *sched_recurrence_to_str(sched_recurrence_t recurrence);
sched_recurrence_t sched_recurrence_from_str(const char *str);

const char *sched_source_type_to_str(sched_source_type_t type);
sched_source_type_t sched_source_type_from_str(const char *str);

/* =============================================================================
 * CRUD Operations
 * ============================================================================= */

/**
 * @brief Insert a new scheduled event
 *
 * IMPORTANT for briefing-type events: post-v50, callers that insert briefings
 * MUST also populate briefing_steps for the new row (use
 * scheduler_db_briefing_steps_set or scheduler_db_insert_with_step_clone for
 * the recurrence-chain path).  A briefing row with no steps and no legacy
 * tool_name will silently no-op at fire time.
 *
 * @param event Event to insert (id and created_at are set by this function)
 * @param id_out Output: event ID on success
 * @return SCHED_DB_SUCCESS or SCHED_DB_FAILURE
 */
int scheduler_db_insert(sched_event_t *event, int64_t *id_out);

/**
 * @brief Atomically check limits and insert event (TOCTOU-safe)
 * @param event Event to insert
 * @param max_per_user Maximum events per user
 * @param max_total Maximum total events
 * @param id_out Output: event ID on success
 * @return SCHED_DB_SUCCESS, SCHED_DB_FAILURE, SCHED_DB_USER_LIMIT, or SCHED_DB_GLOBAL_LIMIT
 */
int scheduler_db_insert_checked(sched_event_t *event,
                                int max_per_user,
                                int max_total,
                                int64_t *id_out);

/**
 * @brief Get event by ID
 * @param id Event ID
 * @param event Output event struct
 * @return SUCCESS or FAILURE if not found
 */
int scheduler_db_get(int64_t id, sched_event_t *event);

/**
 * @brief Update event status
 * @param id Event ID
 * @param status New status
 * @return SUCCESS or FAILURE
 */
int scheduler_db_update_status(int64_t id, sched_status_t status);

/**
 * @brief Update event status with fired_at timestamp
 * @param id Event ID
 * @param status New status
 * @param fired_at Fired timestamp
 * @return SUCCESS or FAILURE
 */
int scheduler_db_update_status_fired(int64_t id, sched_status_t status, time_t fired_at);

/**
 * @brief Update fire_at for snooze (also updates snoozed_until and snooze_count)
 * @param id Event ID
 * @param new_fire_at New fire time
 * @return SUCCESS or FAILURE
 */
int scheduler_db_snooze(int64_t id, time_t new_fire_at);

/**
 * @brief Cancel an event (optimistic: only if still pending/snoozed)
 * @param id Event ID
 * @return SUCCESS if cancelled, FAILURE if already fired/cancelled
 */
int scheduler_db_cancel(int64_t id);

/**
 * @brief Dismiss a ringing event (optimistic: only if status='ringing')
 * @param id Event ID
 * @return SUCCESS if dismissed, FAILURE if already handled
 */
int scheduler_db_dismiss(int64_t id);

/**
 * @brief Mark a missed event as acknowledged (status='missed' → 'dismissed')
 *
 * Used by the WebUI scheduler panel when the user clicks Acknowledge on a
 * missed row or Clear All Missed in the footer.  The row is preserved for
 * eventual cleanup_old_events but no longer surfaces in the queue.  SQL
 * enforces user ownership as defense in depth — callers should still
 * authorize at the dispatcher layer.
 *
 * @param id Event ID
 * @param user_id Owning user (must match the row's user_id)
 * @return SUCCESS if cleared, FAILURE if row was not owned by this user or
 *         not in 'missed' status
 */
int scheduler_db_clear_missed(int64_t id, int user_id);

/* =============================================================================
 * Query Operations
 * ============================================================================= */

/**
 * @brief Get the next fire_at time for pending events
 * @return Next fire_at timestamp, or 0 if no pending events
 */
time_t scheduler_db_next_fire_time(void);

/**
 * @brief Get all events that should fire now (fire_at <= now, status=pending/snoozed)
 * @param events Output array
 * @param max_count Maximum events to return
 * @return Number of events found
 */
int scheduler_db_get_due_events(sched_event_t *events, int max_count);

/**
 * @brief List events for a user filtered by status and optional type
 * @param user_id User ID
 * @param type Event type filter (-1 for all types)
 * @param events Output array
 * @param max_count Maximum events to return
 * @return Number of events found
 */
int scheduler_db_list_user_events(int user_id, int type, sched_event_t *events, int max_count);

/**
 * @brief List `missed`-status events for a user, newest first
 *
 * Distinct from scheduler_db_get_missed_events (which is the DB-wide
 * recovery-sweep helper looking for unprocessed pending/snoozed rows past
 * fire_at).  This returns rows whose status is currently 'missed' — the ones
 * marked missed by startup recovery and not yet acknowledged.
 *
 * @param user_id User ID
 * @param events Output array
 * @param max_count Maximum events to return
 * @return Number of events found
 */
int scheduler_db_list_user_missed(int user_id, sched_event_t *events, int max_count);

/**
 * @brief Find event by name for a user (case-insensitive)
 * @param user_id User ID
 * @param name Event name
 * @param event Output event struct
 * @return SUCCESS or FAILURE if not found
 */
int scheduler_db_find_by_name(int user_id, const char *name, sched_event_t *event);

/**
 * @brief Count pending events for a user
 * @param user_id User ID
 * @param count_out Output: event count on success
 * @return SCHED_DB_SUCCESS or SCHED_DB_FAILURE
 */
int scheduler_db_count_user_events(int user_id, int *count_out);

/**
 * @brief Count total pending events across all users
 * @param count_out Output: event count on success
 * @return SCHED_DB_SUCCESS or SCHED_DB_FAILURE
 */
int scheduler_db_count_total_events(int *count_out);

/**
 * @brief Get currently ringing events (status='ringing')
 * @param events Output array
 * @param max_count Maximum events to return
 * @return Number of events found
 */
int scheduler_db_get_ringing(sched_event_t *events, int max_count);

/**
 * @brief Get active timers for a specific satellite UUID
 * @param uuid Satellite UUID
 * @param events Output array
 * @param max_count Maximum events
 * @return Number of events found
 */
int scheduler_db_get_active_by_uuid(const char *uuid, sched_event_t *events, int max_count);

/**
 * @brief Clean up old fired/cancelled/missed events
 * @param retention_days Delete events older than this many days
 * @param deleted_out Output: number of events deleted on success
 * @return SCHED_DB_SUCCESS or SCHED_DB_FAILURE
 */
int scheduler_db_cleanup_old_events(int retention_days, int *deleted_out);

/**
 * @brief Get all pending/snoozed events that should have fired (for missed recovery)
 * @param events Output array
 * @param max_count Maximum events to return
 * @return Number of events found
 */
int scheduler_db_get_missed_events(sched_event_t *events, int max_count);

/* =============================================================================
 * Briefing Steps (schema v50+)
 *
 * Multi-step briefings store one row per step in the briefing_steps table.
 * Single-step briefings created before v50 backfilled their single step from
 * the scheduled_events legacy tool_name/tool_action/tool_value fields.
 * ============================================================================= */

/**
 * @brief Replace all steps for a briefing event
 *
 * Deletes any existing steps for event_id then inserts the given list.  Used
 * at create time and during recurrence-chain clone.  Wrapped in a single
 * transaction so the steps list always reads consistently.
 *
 * @param event_id Briefing event ID
 * @param steps Step list
 * @param step_count Number of steps (max SCHED_BRIEFING_STEPS_MAX)
 * @return SCHED_DB_SUCCESS or SCHED_DB_FAILURE
 */
int scheduler_db_briefing_steps_set(int64_t event_id,
                                    const sched_briefing_step_t *steps,
                                    int step_count);

/**
 * @brief Ownership- and status-guarded edit of a briefing's steps (append or replace)
 *
 * The guarded editor behind the scheduler `update` action.  Unlike
 * scheduler_db_briefing_steps_set (an unconditional writer used by create + the
 * recurrence clone), this performs the guard AND the write inside a single
 * lock + BEGIN IMMEDIATE, so a fire-thread transition (pending → fired + clone)
 * cannot race between the caller's check and the write and silently drop the
 * edit.  Within the transaction it:
 *   1. reads status, user_id, event_type for @p event_id;
 *   2. rejects with SCHED_DB_NOT_EDITABLE unless the row is owned by @p user_id,
 *      is a briefing, and is still pending or snoozed;
 *   3. on @p append, seeds the list from the current steps — materializing a
 *      pre-v50 legacy single-tool briefing's tool_name/action/value as step[0]
 *      first (otherwise appending would orphan the original tool, since the
 *      steps table takes total precedence at fire time) — then appends @p steps;
 *      on replace, uses @p steps as the whole list;
 *   4. caps the result at SCHED_BRIEFING_STEPS_MAX (SCHED_DB_FAILURE on overflow);
 *   5. replaces the stored steps atomically.
 * On append, a new step exactly matching an existing (tool_name, tool_action,
 * tool_value) is skipped as a duplicate (idempotent LLM retry-after-timeout).
 *
 * @param event_id Briefing event ID
 * @param user_id  Owning user (must match the row's user_id)
 * @param steps    Steps to append or the full replacement list (already validated)
 * @param step_count Number of steps in @p steps
 * @param append   true = append to existing steps, false = replace all
 * @return SCHED_DB_SUCCESS, SCHED_DB_NOT_EDITABLE (guard failed),
 *         or SCHED_DB_FAILURE (DB error or step cap exceeded)
 */
int scheduler_db_briefing_steps_update(int64_t event_id,
                                       int user_id,
                                       const sched_briefing_step_t *steps,
                                       int step_count,
                                       bool append);

/* Field mask bits for scheduler_db_update_fields — only the masked columns are
 * written.  ORIGINAL_TIME is set together with FIRE_AT by the tool layer (the
 * recurrence engine derives time-of-day from original_time), never independently. */
#define SCHED_FIELD_NAME (1u << 0)
#define SCHED_FIELD_MESSAGE (1u << 1)
#define SCHED_FIELD_FIRE_AT (1u << 2)
#define SCHED_FIELD_ORIGINAL_TIME (1u << 3)
#define SCHED_FIELD_RECURRENCE (1u << 4)
#define SCHED_FIELD_RECURRENCE_DAYS (1u << 5)
#define SCHED_FIELD_DELIVER_TO (1u << 6)
#define SCHED_FIELD_SAY_ALOUD (1u << 7)

/**
 * @brief Ownership- and status-guarded scalar-field edit of a scheduled event
 *
 * The scalar half of the `update` action (steps are handled by
 * scheduler_db_briefing_steps_update).  Writes ONLY the columns named in
 * @p field_mask, in a single UPDATE guarded by
 * `WHERE id=? AND user_id=? AND status IN ('pending','snoozed')` — so it edits
 * only a row the caller owns that has not yet fired.  Applies to any event type
 * (alarm/reminder/task/briefing); per-occurrence edits of a recurring series are
 * out of scope (the pending row is the series handle).
 *
 * @param id       Event ID
 * @param user_id  Owning user (SQL-enforced)
 * @param fields   Source values; only masked members are read
 * @param field_mask OR of SCHED_FIELD_* bits (0 → SCHED_DB_FAILURE)
 * @return SCHED_DB_SUCCESS, SCHED_DB_NOT_EDITABLE (no owned/editable row matched),
 *         or SCHED_DB_FAILURE (bad args or DB error)
 */
int scheduler_db_update_fields(int64_t id,
                               int user_id,
                               const sched_event_t *fields,
                               uint32_t field_mask);

/**
 * @brief List steps for a briefing event in seq order
 *
 * count_out == 0 means "no steps stored" — caller falls back to the legacy
 * single-tool fields on the scheduled_events row (pre-v50 backfilled row).
 *
 * @param event_id Briefing event ID
 * @param out Output array
 * @param max_count Maximum steps to return (typically SCHED_BRIEFING_STEPS_MAX)
 * @param count_out Output: number of steps written to out[]
 * @return SCHED_DB_SUCCESS or SCHED_DB_FAILURE
 */
int scheduler_db_briefing_steps_list(int64_t event_id,
                                     sched_briefing_step_t *out,
                                     int max_count,
                                     int *count_out);

/**
 * @brief Atomically insert a recurrence-chain successor event AND clone its
 *        source's briefing_steps in a single BEGIN IMMEDIATE transaction.
 *
 * Used by schedule_next_occurrence for SCHED_EVENT_BRIEFING events so we
 * never end up with a zero-step pending briefing row.  For non-briefing
 * events, call scheduler_db_insert directly.
 *
 * @param next Event to insert (id + created_at set by this function)
 * @param src_event_id Event to clone steps from
 * @param new_id_out Output: new event ID on success
 * @return SCHED_DB_SUCCESS or SCHED_DB_FAILURE
 */
int scheduler_db_insert_with_step_clone(sched_event_t *next,
                                        int64_t src_event_id,
                                        int64_t *new_id_out);

/**
 * @brief Atomically cancel a pending/snoozed row AND insert its next-occurrence
 *        successor in a single BEGIN IMMEDIATE transaction.
 *
 * Closes the chain-break window between scheduler_db_cancel and the follow-up
 * insert_with_step_clone / insert (caller's previous two-call pattern left a
 * window where cancel succeeded but insert failed, silently breaking the
 * recurrence chain).  Under this entry point either both succeed or neither
 * does.
 *
 * If the cancel UPDATE matches 0 rows (row already fired/cancelled/missed),
 * the function returns FAILURE without inserting — avoids scheduling a phantom
 * successor for a row that was already handled by the fire path or another
 * cancel.
 *
 * When clone_steps is true the caller is expected to be cancelling a briefing;
 * the function reads briefing_steps for src_event_id and clones them onto the
 * new row's id inside the same transaction.  Step-clone failure rolls back the
 * whole operation.
 *
 * @param cancel_id Event id to cancel (must be pending/snoozed)
 * @param next Event template for the successor row (id + created_at set on success)
 * @param src_event_id Event id to read briefing_steps from when clone_steps=true
 * @param clone_steps When true, copy briefing_steps from src_event_id to the new row
 * @param new_id_out Output: id of the inserted successor on success, 0 on failure
 * @return SCHED_DB_SUCCESS only when cancel AND insert (AND step-clone if asked) succeeded
 */
int scheduler_db_cancel_and_insert_next(int64_t cancel_id,
                                        sched_event_t *next,
                                        int64_t src_event_id,
                                        bool clone_steps,
                                        int64_t *new_id_out);

/**
 * @brief Batched variant of scheduler_db_briefing_steps_list — looks up steps
 *        for n_events ids under a single auth_db lock acquisition.
 *
 * The single-row helper acquires the auth_db mutex once per call, so a panel
 * render that walks N events with embedded multi-step briefings pays N+1 lock
 * cycles.  This variant collapses that to 1.  At SCHED_MAX_RESULTS=50 the
 * difference is dominant on Pi-class hardware where the mutex backs an NFS or
 * SD-card-resident SQLite file.
 *
 * out_steps must be sized exactly n_events * SCHED_BRIEFING_STEPS_MAX slots
 * (a flat 2D array indexed as `out_steps[i * SCHED_BRIEFING_STEPS_MAX + j]`).
 * out_counts must be sized n_events; written with the per-event step count.
 * Non-briefing event_ids yield count=0 with no error — callers should still
 * filter their array by event_type before calling for clarity, but this is a
 * graceful no-op.
 *
 * @param event_ids Array of event ids to query (length n_events)
 * @param n_events Number of ids in event_ids (> 0)
 * @param out_steps Flat output array sized [n_events][SCHED_BRIEFING_STEPS_MAX]
 * @param out_counts Per-event step count, length n_events
 * @return SCHED_DB_SUCCESS or SCHED_DB_FAILURE
 */
int scheduler_db_briefing_steps_list_many(const int64_t *event_ids,
                                          int n_events,
                                          sched_briefing_step_t *out_steps,
                                          int *out_counts);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULER_DB_H */
