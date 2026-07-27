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
 * Scheduler - Background thread for timers, alarms, reminders, and tasks
 *
 * Uses pthread_cond_timedwait with CLOCK_MONOTONIC for efficient scheduling.
 * Wakes only when the next event is due or when notified of new events.
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#include "core/scheduler_db.h"

/* Forward decl for scheduler_send_tts_to_session below; avoids pulling
 * core/session_manager.h into every scheduler consumer. */
struct session;
typedef struct session session_t;

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

/**
 * @brief Initialize the scheduler subsystem
 *
 * Creates the condvar with CLOCK_MONOTONIC, generates alarm chime PCM,
 * and starts the background scheduler thread.
 *
 * @return 0 on success, non-zero on error
 */
int scheduler_init(void);

/**
 * @brief Shut down the scheduler
 *
 * Signals the scheduler thread to stop and waits for it to join.
 * Frees alarm sound buffers.
 */
void scheduler_shutdown(void);

/* =============================================================================
 * Event Notification
 * ============================================================================= */

/**
 * @brief Notify the scheduler that a new event was created
 *
 * Wakes the scheduler thread so it can recalculate its next wake time.
 * Call this after inserting a new event into the database.
 */
void scheduler_notify_new_event(void);

/**
 * @brief Speak an ad-hoc "now" alert, reusing the scheduler's TTS routing —
 *        WITHOUT a persisted event or the ringing/dismiss state machine.
 *
 * Synthesizes a transient (id = 0, never stored) event and speaks @text verbatim
 * (no "Reminder:" prefix) via the same TTS routing a fired reminder uses; also
 * delivers to a messaging channel when @deliver_to is set.  Does NOT emit a
 * WebUI banner — the caller owns any visual surface (SAGE uses its own
 * attention_alert channel).  Built for the SAGE proactive-attention layer.
 *
 * @param user_id      Owner the alert is delivered to.
 * @param text         The message to speak / show (verbatim).
 * @param type         Event type carried on the transient event (informational).
 * @param deliver_to   Optional messaging channel; when set, delivery is
 *                     EXCLUSIVE to it (suppresses local TTS + banner), matching
 *                     the scheduler's deliver_to semantics.  Pass "" or NULL for
 *                     the default local speaker + WebUI banner.
 * @param speak        true to speak @text aloud; false to suppress voice.  This
 *                     function emits NO WebUI banner in either case — the caller
 *                     owns any visual surface.  Ignored when @deliver_to routes
 *                     to a messaging channel.
 * @return SUCCESS, or FAILURE on bad input — or, when @deliver_to names a
 *         channel, when that channel did not accept the message.
 */
int scheduler_emit_alert(int user_id,
                         const char *text,
                         sched_event_type_t type,
                         const char *deliver_to,
                         bool speak);

/* =============================================================================
 * Ringing State Queries
 * ============================================================================= */

/**
 * @brief Check if any alarm is currently ringing
 * @return true if an alarm is actively ringing
 */
bool scheduler_is_ringing(void);

/**
 * @brief Get the currently ringing event (if any)
 * @param event Output event struct (filled if ringing)
 * @return SUCCESS if ringing event found, FAILURE if nothing ringing
 */
int scheduler_get_ringing(sched_event_t *event);

/**
 * @brief Dismiss the currently ringing alarm
 * @param event_id Event ID to dismiss (0 = dismiss whatever is ringing)
 * @return SUCCESS or FAILURE if nothing to dismiss
 */
int scheduler_dismiss(int64_t event_id);

/**
 * @brief Snooze the currently ringing alarm
 * @param event_id Event ID to snooze (0 = snooze whatever is ringing)
 * @param snooze_minutes Minutes to snooze (0 = use default from config)
 * @return SUCCESS or FAILURE if nothing to snooze
 */
int scheduler_snooze(int64_t event_id, int snooze_minutes);

/**
 * @brief Cancel a single occurrence of an event, keeping the recurrence chain
 *
 * For one-shot events, behaves identically to scheduler_db_cancel.  For
 * recurring events, cancels the current pending/snoozed row AND inserts the
 * next-occurrence row so the daily/weekly chain stays alive.  Use
 * scheduler_db_cancel directly when the caller wants to break the chain.
 *
 * @param id Event ID
 * @return SUCCESS if cancelled (next occurrence scheduled when recurring),
 *         FAILURE if the row was already non-pending or didn't exist.
 */
int scheduler_cancel_occurrence(int64_t id);

/**
 * @brief Cancel an event (series) and broadcast scheduler_events_changed.
 *
 * Thin wrapper used by the WebUI dispatcher and the LLM scheduler tool so
 * the broadcast emission lives in one place.  Cancels via scheduler_db_cancel
 * (no next-occurrence — recurrence chain breaks intentionally).
 */
int scheduler_cancel_and_broadcast(int64_t id, int user_id);

/**
 * @brief Cancel just this occurrence (recurrence chain preserved) and broadcast.
 */
int scheduler_cancel_occurrence_and_broadcast(int64_t id, int user_id);

/* =============================================================================
 * Notification Broadcast Hooks
 *
 * Weak default-stubs live in scheduler.c so the scheduler builds without
 * the WebUI.  Strong overrides live in src/webui/webui_broadcasts.c and
 * fan out via the active-connection registry.  Called from scheduler
 * fire/dismiss paths AND from the WebUI dispatcher when rebroadcasting
 * after a no-op dismiss.
 * ============================================================================= */

void scheduler_broadcast_notification(const sched_event_t *event, const char *text);
void scheduler_broadcast_briefing_notification(const sched_event_t *event,
                                               const char *text,
                                               int64_t conversation_id);
int scheduler_route_tts_to_user(int user_id,
                                const char *text,
                                const char *skip_uuid,
                                int skip_user_id);

/* Fires whenever an event is created / cancelled / fires / dismissed /
 * recovered-missed.  WebUI panels refetch on receipt; the broadcast payload
 * is intentionally empty so we don't ship per-row state through the push
 * channel.  user_id <= 0 means "system event, fan out to all sessions". */
void scheduler_broadcast_events_changed(int user_id);

/* Weak/strong override sibling to scheduler_broadcast_*.  Strong definition
 * lives in src/webui/webui_audio.c so TTS can play through an active WebUI
 * session; the scheduler.c stub is the no-op default. */
void scheduler_send_tts_to_session(session_t *session, const char *text);

/* Fan briefing/task announcements out to a messaging channel (Telegram,
 * Discord, Slack, SMS) in addition to the existing TTS + banner path.
 * Called from the scheduler fire path when `event->deliver_to` is non-
 * empty.  Strong definition lives in src/messaging/messaging_engine.c
 * and delegates to messaging_engine_send, which enforces the
 * (user_id, channel_name) ownership check + per-channel + provider-
 * global rate limits; the scheduler.c stub is the no-op default for
 * messaging-disabled builds.  Sixth scheduler weak symbol — see
 * docs/MESSAGING_CHANNELS_DESIGN.md §9.
 *
 * Returns SUCCESS only if the message actually went out, so a caller that owes
 * an at-least-once delivery (background-job completion notices) can retry rather
 * than record an unsent notice as delivered.  Engine result codes are collapsed
 * to SUCCESS/FAILURE deliberately — the scheduler is not a messaging module and
 * should not carry MESSAGING_* into its callers. */
int scheduler_send_to_messaging_channel(int user_id, const char *channel_name, const char *text);

/* =============================================================================
 * Alarm Sound
 * ============================================================================= */

/**
 * @brief Stop the currently playing alarm sound
 *
 * Called from dismiss/snooze handlers to immediately stop the sound.
 */
void scheduler_stop_alarm_sound(void);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULER_H */
