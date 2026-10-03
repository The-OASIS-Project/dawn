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
 * Messaging engine — Layer 2.
 *
 * Driver registry, per-user channel resolution, link-code issuance, rate
 * limiters (inbound + outbound), worker-drain dispatch.  Public API for
 * the LLM tool and the WebUI link-code endpoint.  See
 * docs/MESSAGING_CHANNELS_DESIGN.md.
 */
#ifndef MESSAGING_ENGINE_H
#define MESSAGING_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "messaging/messaging_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes (use SUCCESS/FAILURE from dawn_error.h elsewhere; these
 * are the specific cases the engine reports). */
#define MESSAGING_SUCCESS 0
#define MESSAGING_FAILURE 1
#define MESSAGING_UNKNOWN_CHANNEL 2
#define MESSAGING_UNKNOWN_USER 3
#define MESSAGING_RATE_LIMITED 4
#define MESSAGING_PROVIDER_RATE_LIMITED 5
#define MESSAGING_DRIVER_NOT_REGISTERED 6
#define MESSAGING_INVALID_ADDRESS 7
#define MESSAGING_NAME_TAKEN 8     /* rename target collides with an existing channel */
#define MESSAGING_ALREADY_LINKED 9 /* this chat is already linked for this person */
#define MESSAGING_BAD_CODE 10      /* SMS verification code wrong, expired or used up */
#define MESSAGING_NOT_VERIFIED 11  /* an SMS link not proven by its code: /link it again */
#define MESSAGING_INVALID_NAME 12  /* a display name too long or with unsafe characters */

/* Maximum channel display_name length (bytes, excluding NUL).  The admin
 * protocol's ADMIN_MESSAGING_DISPLAY_NAME_MAX must match this. */
#define MESSAGING_DISPLAY_NAME_MAX 64

/* Link-code shape — Crockford base32, no I/L/O/U. */
#define MESSAGING_LINK_CODE_LEN 8
#define MESSAGING_LINK_CODE_BUF_SIZE 16 /* room for "DAWN" prefix later if added */
#define MESSAGING_LINK_TTL_SECONDS (10 * 60)

/** Link-code lookup result. */
typedef enum {
   MESSAGING_LINK_STATE_NOT_FOUND = 0,
   MESSAGING_LINK_STATE_PENDING,
   MESSAGING_LINK_STATE_CLAIMED,
   MESSAGING_LINK_STATE_EXPIRED,
} messaging_link_state_t;

/**
 * @brief Initialize the messaging engine.
 *
 * Spawns the worker drain thread, initializes rate limiters, primes the
 * link-code RNG.  Drivers register themselves separately via
 * messaging_engine_register_driver() at their own init time (after the
 * engine but before they go live).
 *
 * @return MESSAGING_SUCCESS on success, MESSAGING_FAILURE if init fails.
 */
int messaging_engine_init(void);

/**
 * @brief Shut the engine down.  Stops the worker drain.  Drivers are
 *        torn down separately by their own shutdown calls.
 */
void messaging_engine_shutdown(void);

/**
 * @brief Register a driver with the engine.
 *
 * Called by each driver at its init time, AFTER messaging_engine_init().
 * The engine takes a copy of the function pointers but does not own the
 * memory backing the driver struct (drivers are static const).
 *
 * @return MESSAGING_SUCCESS / MESSAGING_FAILURE (registry full or
 *         duplicate name).
 */
int messaging_engine_register_driver(const messaging_driver_t *driver);

/**
 * @brief Send a text message on a named channel for a specific user.
 *
 * Looks up the channel in messaging_channels by (user_id, display_name),
 * verifies ownership, applies per-channel + provider-global rate limit,
 * dispatches to the driver.
 *
 * @param user_id       The DAWN user the LLM tool is acting on behalf of.
 * @param channel_name  Display name as registered (e.g., "telegram_main").
 *                      Case-insensitive match.
 * @param text          UTF-8 message body (v1 text-only, no formatting).
 *
 * @return MESSAGING_SUCCESS, MESSAGING_UNKNOWN_CHANNEL,
 *         MESSAGING_RATE_LIMITED, MESSAGING_PROVIDER_RATE_LIMITED,
 *         MESSAGING_DRIVER_NOT_REGISTERED, MESSAGING_FAILURE.
 */
int messaging_engine_send(int user_id, const char *channel_name, const char *text);

/**
 * @brief Options for messaging_engine_read_channel().
 *
 * `channel_name` is required; everything else is optional (zero-init = the
 * sensible default).  `since_ts`/`until_ts` are Unix-seconds bounds (0 =
 * unbounded / up to now); `before_id` is an exact older-history cursor (a
 * provider message id, NULL/"" = none); `limit` <= 0 = default; `server_hint`
 * disambiguates a name shared across servers (NULL = none).
 */
typedef struct {
   const char *channel_name;
   int64_t since_ts;
   int64_t until_ts;
   const char *before_id;
   int limit;
   const char *server_hint;
} messaging_read_channel_opts_t;

/**
 * @brief Options for messaging_engine_read_server().
 *
 * All optional (zero-init = every readable channel, every recent message).
 * `server_hint` NULL = auto-select when the bot is in exactly one server.
 * `channels`/`channel_count` restrict to an explicit fuzzy-matched subset
 * (NULL/0 = all) — used to target a few channels or fetch "the rest" after a
 * truncated sweep.  `since_ts`/`until_ts` as above.
 */
typedef struct {
   const char *server_hint;
   int64_t since_ts;
   int64_t until_ts;
   const char *const *channels;
   int channel_count;
} messaging_read_server_opts_t;

/**
 * @brief Read recent messages from a named channel and return a transcript.
 *
 * Resolves @p opts->channel_name against the driver's discoverable (bot-visible)
 * channels via fuzzy match — distinct from messaging_engine_send, which
 * resolves against the user's *linked* channels.  Runs each message body
 * through the injection filter and formats a chronological, `[DATA]`-wrapped
 * transcript for the LLM to summarize; the transcript surfaces the oldest-shown
 * message id as the next older-history cursor.  Discord-only in v1.  Applies a
 * per-user read rate limit and an audit log line.
 *
 * @param user_id  DAWN user the read is on behalf of (rate-limit + audit key).
 *                 Must be > 0.
 * @param opts     Read options (see messaging_read_channel_opts_t).  Must be
 *                 non-NULL with a non-empty channel_name.
 * @param out_text On MESSAGING_SUCCESS, set to an allocated transcript (or a
 *                 disambiguation/empty-result message).  Caller frees.
 *                 Untouched on failure.
 *
 * @return MESSAGING_SUCCESS, MESSAGING_UNKNOWN_CHANNEL (no fuzzy match),
 *         MESSAGING_RATE_LIMITED, MESSAGING_DRIVER_NOT_REGISTERED (no
 *         read-capable driver), MESSAGING_FAILURE.
 */
int messaging_engine_read_channel(int user_id,
                                  const messaging_read_channel_opts_t *opts,
                                  char **out_text);

/**
 * @brief Read recent messages from the readable channels of one server and
 *        return a single per-channel-sectioned transcript to summarize.
 *
 * Resolves the target server (by @p opts->server_hint, or automatically when
 * the bot is in only one), then sweeps its text/announcement channels (or the
 * `channels` subset) — each a `## #channel` section inside one `[DATA]`
 * envelope, with the same per-message injection filtering as
 * messaging_engine_read_channel.  Bounded by a channel cap, a per-channel
 * message cap, and a total transcript budget; quiet channels are shown as
 * "(no recent activity)".  Discord-only in v1.
 *
 * @param user_id  DAWN user the read is on behalf of (rate-limit + audit).
 * @param opts     Read options (see messaging_read_server_opts_t).  Must be
 *                 non-NULL.
 * @param out_text On MESSAGING_SUCCESS, set to an allocated transcript (or a
 *                 disambiguation message).  Caller frees.
 *
 * @return MESSAGING_SUCCESS, MESSAGING_UNKNOWN_CHANNEL (no servers visible),
 *         MESSAGING_RATE_LIMITED, MESSAGING_DRIVER_NOT_REGISTERED,
 *         MESSAGING_FAILURE.
 */
int messaging_engine_read_server(int user_id,
                                 const messaging_read_server_opts_t *opts,
                                 char **out_text);

/**
 * @brief List the bot-visible Discord channels (no message fetch) so the LLM
 *        can discover the server layout cheaply before deciding what to read.
 *
 * Returns the text/announcement channels of every server the bot has joined,
 * grouped by server, using the driver's discovery cache — does NOT fetch any
 * message history (so it's cheap), though it is still counted against the
 * per-user read rate limit.  Optional @p server_hint filters to one server
 * (fuzzy).  Discord-only in v1.
 *
 * @param user_id      DAWN user (rate-limit + audit).  Must be > 0.
 * @param server_hint  Optional server name filter.  May be NULL.
 * @param out_text     On MESSAGING_SUCCESS, an allocated channel listing (or a
 *                     "no servers" message).  Caller frees.
 *
 * @return MESSAGING_SUCCESS, MESSAGING_RATE_LIMITED,
 *         MESSAGING_DRIVER_NOT_REGISTERED, MESSAGING_FAILURE.
 */
int messaging_engine_list_discord_channels(int user_id, const char *server_hint, char **out_text);

/**
 * @brief List a user's channels as a JSON array.
 *
 * Returns an allocated string like
 * `[{"name":"telegram_main","provider":"telegram","enabled":true}, ...]`.
 * Caller frees.
 *
 * @return Allocated JSON string (caller frees), or NULL on failure.
 */
char *messaging_engine_list_channels_json(int user_id);

/**
 * @brief Render a user's channels as an aligned text table.
 *
 * Operator-facing equivalent of messaging_engine_list_channels_json for
 * the `dawn-admin messaging list-channels` command (the WebUI panel uses
 * the JSON form).  Columns: ID, NAME, PROVIDER, ENABLED, LAST USED.
 *
 * @param user_id  Owner of the channels.
 * @param buf      Caller buffer; always NUL-terminated on return.
 * @param buflen   Size of @p buf.
 * @return MESSAGING_SUCCESS / MESSAGING_FAILURE.
 */
int messaging_engine_list_channels_text(int user_id, char *buf, size_t buflen);

/**
 * @brief Generate a new one-time link code for `user_id`.
 *
 * Inserts into messaging_link_codes with a 10-minute TTL.  Prunes
 * expired codes opportunistically.
 *
 * @param user_id       Owner of the code (must exist in users).
 * @param provider_hint Optional ("telegram"/"discord"/"slack"/"sms"): the
 *                      code then links only that app.  NULL/empty for any.
 * @param code_out      Caller-allocated buffer.
 * @param code_buf_size Must be >= MESSAGING_LINK_CODE_BUF_SIZE.
 *
 * @return MESSAGING_SUCCESS / MESSAGING_RATE_LIMITED (the user already holds
 *         several live codes) / MESSAGING_FAILURE.
 */
int messaging_engine_generate_link_code(int user_id,
                                        const char *provider_hint,
                                        char *code_out,
                                        size_t code_buf_size);

/**
 * @brief Look up the current state of a link code.
 *
 * @param code The code to check (case-insensitive).
 * @return MESSAGING_LINK_STATE_*.
 */
messaging_link_state_t messaging_engine_link_status(const char *code);

/**
 * @brief Finish linking an SMS number with the code DAWN texted to it.
 *
 * `/link CODE` from a number only proves someone can put that number on a
 * text (the sender can be forged); entering the code DAWN texted back proves
 * the user receives texts there.  Until then the channel is pending: listed,
 * but it reaches nothing.  Call from an authenticated surface (the WebUI).
 *
 * @return MESSAGING_SUCCESS; MESSAGING_BAD_CODE (wrong, expired, or too many
 *         tries — ask for a new code with messaging_engine_resend_verify_code);
 *         MESSAGING_UNKNOWN_CHANNEL (no pending channel by that id for the
 *         user); MESSAGING_ALREADY_LINKED (another account verified the
 *         number); MESSAGING_FAILURE.
 */
int messaging_engine_verify_channel(int user_id, int64_t channel_id, const char *code);

/**
 * @brief Text a fresh verification code to a channel waiting for one.
 *
 * Codes sent are limited per channel and per user over a day (each resets
 * the attempt count, so the limit is what bounds guessing).
 *
 * @return MESSAGING_SUCCESS; MESSAGING_RATE_LIMITED (today's codes are used
 *         up); MESSAGING_UNKNOWN_CHANNEL (no channel waiting for a code);
 *         MESSAGING_FAILURE.
 */
int messaging_engine_resend_verify_code(int user_id, int64_t channel_id);

/**
 * @brief The code part of a `/link CODE` message, or NULL if the text isn't
 *        one.
 *
 * Accepts "/link CODE" in any letter case, after leading spaces, Telegram's
 * "/link@BotName CODE", and "link CODE" (Slack reserves '/') when CODE has a
 * link code's shape.  One matcher for every place that must recognize a link
 * code (the engine, and the phone service keeping it out of the SMS log).
 */
const char *messaging_link_command_args(const char *body);

/**
 * @brief Reset a channel by its display_name for a specific user.
 *
 * Clears messaging_channels.conversation_id back to NULL so the next
 * inbound for this channel starts a fresh conversations row.  Evicts
 * the in-memory session slot for the channel (if any), which triggers
 * memory extraction on the closing conversation's tail via the
 * existing session_destroy → memory_trigger_extraction path.  Used by
 * the LLM-facing `messaging.reset_conversation` action (the `/new`
 * command resets the same way, inside the engine).  Looks up the
 * channel by (user_id, channel_name) and enforces the same ownership
 * boundary as `messaging_engine_send`.
 *
 * @param user_id       The DAWN user the LLM tool is acting on behalf of.
 * @param channel_name  Display name as registered.  Case-insensitive.
 *
 * @return MESSAGING_SUCCESS, MESSAGING_UNKNOWN_CHANNEL (no enabled channel by
 *         that name), MESSAGING_FAILURE.
 */
int messaging_engine_reset_by_name(int user_id, const char *channel_name);

/**
 * @brief Unlink (soft-delete) a channel by display_name for a user.
 *
 * Resolves (user_id, display_name) → (provider, provider_address) under
 * the ownership boundary, evicts any live in-memory session slot (which
 * fires memory extraction on the closing conversation), then sets
 * `is_enabled = 0`.  The row and its `conversation_id` are PRESERVED:
 * unlink means "disconnect this surface," not "wipe history."  A later
 * re-link of the same address re-enables the existing row (via the
 * link-claim `ON CONFLICT … DO UPDATE SET is_enabled = 1` upsert) and
 * resumes the prior forever-conversation; the custom display_name also
 * survives.  Inbound on a disabled channel stops dispatching because the
 * inbound lookup filters `is_enabled = 1`.
 *
 * NOTE: no self-reset deferral guard is applied — the admin / WebUI
 * callers have no LLM command context, so they cannot target the channel
 * a running turn is on.  A future user-facing `/unlink` slash command
 * would need to add a deferred-unlink path mirroring reset's
 * `mark_pending_reset_if_self`.
 *
 * @return MESSAGING_SUCCESS, MESSAGING_UNKNOWN_CHANNEL (no enabled
 *         channel by that name for the user), MESSAGING_FAILURE.
 */
int messaging_engine_unlink_channel(int user_id, const char *display_name);

/**
 * @brief Rename a channel's display_name for a user.
 *
 * Rejects a rename whose target collides (case-insensitively) with
 * another enabled channel owned by the same user — name-based lookups
 * (`messaging_engine_send`, `_reset_by_name`) use `LIMIT 1`, so duplicate
 * display_names would silently shadow each other.  A no-op rename
 * (new_name equal to old_name modulo case) is allowed.
 *
 * @return MESSAGING_SUCCESS, MESSAGING_UNKNOWN_CHANNEL (old_name not
 *         found), MESSAGING_NAME_TAKEN (new_name collides),
 *         MESSAGING_FAILURE (bad args / DB error).
 */
int messaging_engine_rename_channel(int user_id, const char *old_name, const char *new_name);

/**
 * @brief Unlink (soft-delete) a channel by stable row id.
 *
 * Same semantics as messaging_engine_unlink_channel but keyed on the
 * `messaging_channels.id` primary key under the (id, user_id) ownership
 * boundary.  Preferred by concurrent surfaces (the WebUI panel) where
 * the mutable, non-unique display_name is a poor operation key.
 *
 * @return MESSAGING_SUCCESS, MESSAGING_UNKNOWN_CHANNEL (no enabled
 *         channel with that id owned by the user), MESSAGING_FAILURE.
 */
int messaging_engine_unlink_channel_by_id(int user_id, int64_t channel_id);

/**
 * @brief Rename a channel by stable row id.
 *
 * Same collision semantics as messaging_engine_rename_channel, keyed on
 * the row id under the (id, user_id) ownership boundary.
 *
 * @return MESSAGING_SUCCESS, MESSAGING_UNKNOWN_CHANNEL, MESSAGING_NAME_TAKEN,
 *         MESSAGING_INVALID_NAME (too long, or quotes, angle brackets,
 *         backslashes or control characters), MESSAGING_FAILURE.
 */
int messaging_engine_rename_channel_by_id(int user_id, int64_t channel_id, const char *new_name);

/**
 * @brief Re-enable a previously unlinked (soft-deleted) channel.
 *
 * The inverse of unlink: flips `is_enabled` back to 1 on a row that was
 * soft-deleted, restoring inbound dispatch.  For a chat app no `/link`
 * round-trip is needed — the row still names its owner, and unlink
 * preserved it (provider_address, address_json, conversation_id,
 * display_name).  An SMS number loses its verification when unlinked (the
 * number may have changed hands), so it comes back only through a new
 * `/link` and code.  Rejects re-enable when it would collide with an
 * existing ENABLED channel of the same display_name (name-based lookups use
 * LIMIT 1).  No session eviction — re-enabling touches no live session.
 *
 * Keys on the row id (preferred for the WebUI).
 *
 * @return MESSAGING_SUCCESS, MESSAGING_UNKNOWN_CHANNEL (no disabled
 *         channel matches), MESSAGING_NAME_TAKEN (name now collides),
 *         MESSAGING_NOT_VERIFIED (link it again), MESSAGING_ALREADY_LINKED
 *         (the same person's chat is live in another account),
 *         MESSAGING_FAILURE.
 */
int messaging_engine_reenable_channel_by_id(int user_id, int64_t channel_id);

/**
 * @brief Re-enable a previously unlinked channel by display_name.
 *
 * By-name form of messaging_engine_reenable_channel_by_id for the operator
 * CLI: resolves the disabled row's id by display_name, then delegates to
 * the by-id path for the collision check + enable.
 *
 * @return Same codes as messaging_engine_reenable_channel_by_id.
 */
int messaging_engine_reenable_channel(int user_id, const char *display_name);

/**
 * @brief Render recent /link attempts as an aligned text table.
 *
 * Operator abuse-review surface (admin-only; no per-user ownership
 * filter — link attempts are pre-link, so there's no owning user yet).
 * Writes a header line plus up to `limit` rows newest-first into @p buf.
 *
 * @param provider_filter  NULL/empty = all providers; else exact match.
 * @param since            Unix epoch lower bound; <= 0 defaults to the
 *                         last 7 days (matching the table's retention).
 *                         NOTE: the admin-socket command currently always
 *                         passes 0 (no `--since` flag in v1); call the API
 *                         directly to use a custom window.
 * @param limit            Max rows; <= 0 or > 200 clamps to 50.
 * @param buf              Caller buffer; always NUL-terminated on return.
 * @param buflen           Size of @p buf.
 *
 * @return MESSAGING_SUCCESS / MESSAGING_FAILURE.
 */
int messaging_engine_link_attempts_text(const char *provider_filter,
                                        int64_t since,
                                        int limit,
                                        char *buf,
                                        size_t buflen);

/**
 * @brief Return true if `name` is a provider name the engine recognizes.
 *
 * Single source of truth for the set of known provider names — call
 * from operator-facing surfaces (admin opcodes, /link confirmations)
 * that need to reject typos before they reach the registry.  Phase 4+
 * additions (Mattermost, Matrix, ...) only need to update the
 * implementation; callers stay unchanged.
 *
 * Case-sensitive match against the canonical lowercase names the
 * drivers register with.  NULL or empty input returns false.
 */
bool messaging_engine_provider_known(const char *name);

/**
 * @brief Try to handle an inbound SMS as a messaging-channels event.
 *
 * Called by phone_service.c on every inbound SMS.  Applies the
 * /link short-circuit, the wake-word prefix gate, and the
 * sender-in-channels check.  If all gates pass, enqueues the
 * (wake-word-stripped) command remainder for LLM dispatch via the
 * worker drain.
 *
 * Return semantics:
 *   - MESSAGING_SUCCESS         — handled here (LLM turn queued).
 *                                 phone_service should SKIP the
 *                                 legacy external-content context
 *                                 injection.
 *   - MESSAGING_UNKNOWN_CHANNEL — not for the LLM (wake word absent
 *                                 or sender not linked).  Caller
 *                                 should fall through to existing
 *                                 HUD popup + context-injection
 *                                 behavior.
 *   - MESSAGING_RATE_LIMITED    — pre-DB or /link rate cap hit.
 *                                 Caller may treat as "drop silently"
 *                                 or fall through; either is safe.
 *   - MESSAGING_FAILURE         — engine not initialized or hard
 *                                 error.  Caller falls through.
 *
 * Always-on regardless of whether the SMS driver is wired into the
 * engine — applies its own provider-name "sms" everywhere.
 */
int messaging_engine_handle_sms_inbound(const char *sender_e164,
                                        const char *sender_display,
                                        const char *body,
                                        int64_t timestamp);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_ENGINE_H */
