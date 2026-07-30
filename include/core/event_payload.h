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
 * Conversation-event payload construction (background jobs Phase 2).  Layer-1 leaf.
 *
 * ONE module owns the three things that must agree about an event payload: its
 * JSON shape, its redaction policy (§8.6), and its size cap (`[jobs]
 * event_chunk_cap`).  The DB layer (auth_db_events.c) deliberately stores what
 * it is given and inspects nothing, so if these three lived at the call sites
 * they would drift per-site — which is exactly how a secret eventually gets
 * persisted.
 *
 * REDACTION POLICY — CONTENT-based, deliberately NOT a name-allowlist and NOT
 * keyed on the tool's TOOL_CAP_SECRETS capability.  An allowlist would render
 * most args "<redacted>", gutting the observe surface Phase 2 exists to build;
 * a capability whole-redact erased benign device/event state (home_assistant,
 * calendar) whose secret lives in secrets.toml and never appears in the args or
 * results.  This is a personal assistant — "the state of my HA" is not a secret.
 * Two cheap rules catch a real credential wherever it appears:
 *
 *   1. key-name pattern  (pass/secret/token/key/credential/auth/bearer)
 *   2. value shape       ("sk-…", "Bearer …", long unbroken base64/hex runs)
 *
 * Applied per-arg on tool_call; tool_result bodies are stored readable (a tool
 * does not echo its own config credential).  Residual: a secret embedded in
 * freeform result text is not scrubbed — close it with a substring scrubber only
 * if a real case appears.  Recorded in docs/BACKGROUND_JOBS_DESIGN.md §8.6.
 */

#ifndef EVENT_PAYLOAD_H
#define EVENT_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Placeholder substituted for a redacted value. */
#define EVENT_REDACTED_MARKER "<redacted>"

/**
 * @brief Truncate @p len to a UTF-8 code-point boundary at or below itself.
 *
 * Inspects the FIRST EXCLUDED byte and walks back, so the kept prefix
 * [0, result) never ends mid-sequence.  Checking the last *included* byte
 * instead stops on a lead byte and keeps an unterminated code point — a real
 * bug that a PR review on this program proposed as a "fix"; verified across
 * every truncation offset of mixed ASCII/2-byte/3-byte input.
 *
 * @param s   Buffer whose byte at @p len (the first EXCLUDED byte) is READ — so
 *            it must be accessible, i.e. the buffer is at least @p len + 1 bytes
 *            (a NUL-terminated C string of length >= @p len satisfies this).
 * @param len Desired cut point.
 * @return Adjusted length, <= @p len.
 */
size_t event_payload_utf8_floor(const char *s, size_t len);

/**
 * @brief Build a redacted, capped `tool_call` payload.
 *
 * @param tool_name Tool being invoked (recorded in the payload for display).
 * @param args_json Raw arguments JSON (LLM-generated); may be NULL.
 * @return malloc'd JSON, or NULL on OOM. Caller frees.
 */
char *event_payload_tool_call(const char *tool_name, const char *args_json);

/**
 * @brief Build a redacted, capped `tool_result` payload.
 *
 * Results are attacker-influenced (web fetches, repo/tool output), so they are
 * capped and stored as an opaque string — never parsed here, and rendered as
 * text only at every consumer (§8.7).
 *
 * @return malloc'd JSON, or NULL on OOM. Caller frees.
 */
char *event_payload_tool_result(const char *tool_name, const char *result_text);

/**
 * @brief Build a `status` payload: {"state":"generating"|"idle"}.
 * @return malloc'd JSON, or NULL on OOM. Caller frees.
 */
char *event_payload_status(bool generating);

/**
 * @brief Build a `complete` payload carrying the terminal disposition (§6.2).
 *
 * @param disposition      done|failed|interrupted|cancelled.
 * @param error_or_null    Failure reason; redacted before persisting (§8.6).
 * @param final_message_id `messages.id` of the final answer, or 0 if none.
 * @return malloc'd JSON, or NULL on OOM. Caller frees.
 */
char *event_payload_complete(const char *disposition,
                             const char *error_or_null,
                             int64_t final_message_id);

/**
 * @brief Build a `spawn` payload: {"conv_id":N,"title":"…"}.
 * @return malloc'd JSON, or NULL on OOM. Caller frees.
 */
char *event_payload_spawn(int64_t child_conv_id, const char *title);

/**
 * @brief True if @p key names a field whose value must never be persisted.
 *
 * Exposed for unit tests and for any future consumer that redacts a structure
 * this module doesn't build.
 */
bool event_payload_key_is_sensitive(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_PAYLOAD_H */
