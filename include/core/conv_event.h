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
 * the project author(s).
 *
 * Conversation-event emit: persist + live fan-out as ONE operation.  Layer 2.
 *
 * Every observe-side event has to do two things — land durably (so a client
 * attaching later can replay it) and reach clients attached right now (so a
 * tailer sees it live).  Splitting that pairing across the emit sites is how
 * one of them eventually persists without broadcasting, producing a job that
 * looks frozen until you reload.  So the pairing lives here, once, and the
 * emit sites are one-liners.
 *
 * The broadcast carries the DB-assigned seq: clients dedup on it when a live
 * frame races the replay batch during attach.
 *
 * Layering: this module sits above auth_db (Layer 2) and reaches the WebUI
 * (Layer 4) only through a weak symbol, matching the
 * job_reinvoke_notify_conv_appended idiom — no upward dependency.
 */

#ifndef CONV_EVENT_H
#define CONV_EVENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event kinds (§6.2).  Kept as named constants so a typo is a link error at the
 * emit site rather than an event kind no consumer recognises. */
#define CONV_EVENT_STATUS "status"
#define CONV_EVENT_TOOL_CALL "tool_call"
#define CONV_EVENT_TOOL_RESULT "tool_result"
#define CONV_EVENT_TERMINAL_CHUNK "terminal_chunk" /* Phase 4 */
#define CONV_EVENT_SPAWN "spawn"
#define CONV_EVENT_COMPLETE "complete"

/**
 * @brief Persist one event and fan it out to the owner's attached clients.
 *
 * TAKES OWNERSHIP of @p payload_owned and frees it on every path, including
 * failure — so a caller can pass an event_payload_*() result inline without a
 * cleanup branch, and an OOM there degrades to "no event" rather than a leak.
 *
 * Failures are logged and swallowed: an observe-side event must never break the
 * turn it is describing.
 *
 * @param conv_id       Conversation the event belongs to.
 * @param user_id       Owner, for broadcast targeting. <= 0 skips the fan-out
 *                      (the event still persists and will be seen on attach).
 * @param kind          One of the CONV_EVENT_* constants.
 * @param payload_owned malloc'd JSON, or NULL. Freed here.
 */
void conv_event_emit(int64_t conv_id, int user_id, const char *kind, char *payload_owned);

/**
 * @brief Push one event to a user's attached clients.
 *
 * Weak default is a no-op (WEBUI-off builds, and unit tests that link this
 * module without the WebUI); webui_broadcasts.c provides the strong override.
 */
void webui_broadcast_conversation_event(int user_id,
                                        int64_t conv_id,
                                        int64_t seq,
                                        const char *kind,
                                        const char *payload);

/**
 * @brief Announce that an assistant message was persisted, WITH its body (§6.3).
 *
 * NOT an entry in conversation_events — deliberately.  Final assistant text
 * lives in `messages`, and the event log stays step-granular; duplicating a
 * multi-KB answer into it would double the storage for no gain.  But that means
 * a consumer tailing ONLY events would watch a job run and never learn what it
 * concluded, which §6 flags as U-Crit.  This frame closes that: it is what makes
 * a dumb line-printer (and the future TUI) a complete client.
 *
 * Distinct from the existing `conversation_messages_appended` broadcast, which
 * is signal-only ("something changed, go refetch") and useless to a client that
 * cannot issue a REST call.
 *
 * Call AFTER the row is durably persisted, with the row id — a client that
 * already received the same text by streaming dedups on it.
 *
 * @param text May be NULL/empty; the frame is then skipped.
 */
void conv_event_notify_message_appended(int64_t conv_id,
                                        int user_id,
                                        int64_t msg_id,
                                        const char *role,
                                        const char *text);

/** Weak seam for the above; strong override in webui_broadcasts.c. */
void webui_broadcast_message_appended(int user_id,
                                      int64_t conv_id,
                                      int64_t msg_id,
                                      const char *role,
                                      const char *text);

#ifdef __cplusplus
}
#endif

#endif /* CONV_EVENT_H */
