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
 * @file pending_system_msg.h
 * @brief Thread-safe queue of deferred "system"-role context messages destined
 *        for the local session's conversation history.
 *
 * @section rationale Design Rationale
 * The local session (sessions[0], the voice pipeline) has a peculiar ownership
 * property: its conversation_history is appended-to by the main thread WITHOUT
 * holding history_mutex (the main path mutates the global alias directly, gated
 * on !llm_processing). That means history_mutex does not actually serialize a
 * foreign thread's append against the main path — the only safe way to add a
 * write from another thread is to move that write onto the main thread too.
 *
 * The phone call-state broadcaster runs on the MQTT/echo callback thread and
 * needs to inject "system" context ("incoming call from X", "call connected")
 * so the LLM is aware on its next turn. Rather than writing sessions[0] from
 * that thread, it enqueues here; the main loop drains the queue on its tick,
 * gated on !llm_processing, so the local history has a single owner at any
 * instant (the worker during a turn, the main thread when idle).
 *
 * This mirrors the input-queue reroute (commit 3af86ba) that moved MQTT
 * device-relay text off the callback thread and onto the main loop.
 *
 * @section overflow Overflow Policy
 * Fixed-capacity ring; when full, the oldest item is dropped (FIFO eviction)
 * with a warning. Text longer than PENDING_SYSMSG_MAX_TEXT is truncated.
 *
 * @section threading Thread Safety
 * All functions are thread-safe via an internal leaf mutex (no other lock is
 * held across these calls). Multiple producers may push; a single consumer
 * (the main loop) drains.
 */

#ifndef PENDING_SYSTEM_MSG_H
#define PENDING_SYSTEM_MSG_H

#include <stddef.h>

/** Maximum length of a deferred system message (device context lines are short). */
#define PENDING_SYSMSG_MAX_TEXT 512

/** Maximum number of queued messages before the oldest is dropped. */
#define PENDING_SYSMSG_MAX_ITEMS 8

/**
 * @brief Enqueue a "system"-role context message for the local session.
 *
 * Safe to call from any thread (e.g. the MQTT/echo callback). The message is
 * applied later by the main loop via pending_sysmsg_pop().
 *
 * @param text Message content (NULL/empty is a no-op returning 0). Truncated to
 *             PENDING_SYSMSG_MAX_TEXT if longer.
 * @return 1 if enqueued, 0 on NULL/empty input.
 */
int pending_sysmsg_push(const char *text);

/**
 * @brief Pop the oldest deferred message (FIFO). Consumer side (main thread).
 *
 * @param out       Caller buffer to receive the null-terminated text.
 * @param out_size  Size of @p out in bytes.
 * @return 1 if a message was copied out, 0 if the queue was empty.
 */
int pending_sysmsg_pop(char *out, size_t out_size);

/**
 * @brief Whether any deferred messages are pending.
 * @return 1 if the queue is non-empty, 0 otherwise.
 */
int pending_sysmsg_has_item(void);

/** @brief Drop all pending messages. */
void pending_sysmsg_clear(void);

#endif /* PENDING_SYSTEM_MSG_H */
