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
 * Messaging engine — INTERNAL shared surface.
 *
 * Private to the src/messaging/messaging_engine*.c translation units.  Holds
 * the cross-file types, module-state externs, and helper prototypes that the
 * engine was split across (core / session / channels / link / inbound) when
 * messaging_engine.c outgrew the 2,500-line hard limit.  NOT a public API —
 * external consumers use include/messaging/messaging_engine.h.  See
 * docs/MESSAGING_ENGINE_SPLIT_PLAN.md for the decomposition rationale.
 *
 * Each engine .c that includes this header must first
 *   #define MESSAGING_ENGINE_INTERNAL_ALLOWED
 * so an accidental include from outside the engine fails the build.
 */
#ifndef MESSAGING_ENGINE_INTERNAL_H
#define MESSAGING_ENGINE_INTERNAL_H

#ifndef MESSAGING_ENGINE_INTERNAL_ALLOWED
#error "messaging_engine_internal.h is private to src/messaging/messaging_engine*.c"
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/rate_limiter.h"
#include "core/session_manager.h"
#include "core/tool_call_challenge.h"
#include "messaging/messaging_engine.h"

struct json_object;

/* =============================================================================
 * Cross-file constants
 *
 * Only the constants referenced from more than one engine .c live here;
 * file-local sizing constants stay in the .c that owns them.
 * ============================================================================= */

#define MESSAGING_INBOUND_QUEUE_DEPTH 32
#define MESSAGING_MAX_SESSIONS 64

/* Stack buffer for per-provider address_json blob construction
 * (build_address_json_for).  Sized for Telegram chat_id + SMS phone_e164 +
 * Discord/Slack extras (guild_id, team_id).  Shared by the channels, link,
 * and inbound files, which all build the blob at their send sites. */
#define MESSAGING_ADDRESS_JSON_BUF_SIZE 256
/* How long shutdown waits for async sends still running (a confirmation or a
 * verification code going out); one that's still running after this is left. */
#define MESSAGING_ASYNC_SEND_DRAIN_MS 5000

/* =============================================================================
 * Cross-file types
 * ============================================================================= */

/* A linked channel, resolved for one inbound message: its row and the DAWN
 * user it belongs to.  Several users may link one chat (each from their own
 * account), so the row, not the chat, names the channel from here on. */
typedef struct {
   int64_t channel_id;
   int user_id;
   char display_name[MESSAGING_DISPLAY_NAME_MAX + 1];
   int64_t last_used_at;      /* 0 = never */
   bool authenticates_sender; /* the provider vouches for who sent (false: SMS) */
} channel_ref_t;

/* Enabled and verified: a channel that may carry messages.  The owner index
 * (schema v101) is defined over exactly these rows. */
#define MESSAGING_LIVE_SQL "is_enabled = 1 AND verified_at IS NOT NULL"

/* What an inbound message is to the engine. */
typedef enum {
   INBOUND_MESSAGE = 0, /* something said: an LLM turn */
   INBOUND_CODE,        /* a reply code while one waits (core/reply_code.h): body is the code */
   INBOUND_STOP,        /* STOP / CANCEL while a code waits: drops the waiting action */
} inbound_kind_t;

typedef struct {
   char provider[16];
   char provider_address[128];
   char sender_display[64];
   char *body; /* heap-allocated */
   int64_t timestamp;
   channel_ref_t ref; /* resolved at enqueue time */
   inbound_kind_t kind;
} inbound_item_t;

typedef struct {
   int64_t channel_id; /* the messaging_channels row this session serves */
   char provider[16];
   char provider_address[128];
   session_t *session;
   time_t last_used;
   /* Self-reset deferral flag.  Set when the LLM (running on THIS
    * slot's session via the tool loop) calls
    * `messaging.reset_conversation` targeting its own channel.
    * Immediate reset would session_destroy the session out from under
    * the worker that's still mid-dispatch.  Instead, the reset is
    * deferred: process_inbound checks this flag after its dispatch
    * returns + persistence completes, then performs the eviction +
    * conv_id clear there.  Reset by the worker after processing. */
   bool pending_reset;
   /* Highest msg_id stamped on a message we (the messaging engine)
    * have written or restored into this slot's session.  Used by the
    * cross-channel staleness check in process_inbound — when an
    * external writer (WebUI conversation panel, voice session, MCP)
    * appends to the same conv between messaging turns, DB MAX(id)
    * exceeds this value and we reload session->conversation_history
    * from DB so the LLM doesn't operate on stale frozen context.
    * Set: (a) during get_or_create_messaging_session's history
    * restore, (b) after each successful conv_db_add_message_ex in
    * process_inbound.  Zero means "no DB persistence yet" — the
    * staleness check correctly no-ops in that case. */
   int64_t last_known_msg_id;
} session_slot_t;

/* Per-provider outbound shaping policy.  `max_outbound_chars` is the
 * engine-side cap (measured on the CONVERTED, per-channel output — see
 * messaging_deliver) before the driver hits the provider's own limit; 0 means
 * no engine cap.  `split_oversize` routes an oversize reply through the
 * formatter/splitter (all v1 providers); false selects hard-truncate (no v1
 * provider).  `max_parts` rejects a reply that would need more than N messages
 * (SMS=3; 0=unlimited).  `channel_hint` is appended to the per-turn system
 * prompt.  Defined in messaging_engine_inbound.c. */
typedef struct {
   size_t max_outbound_chars;
   bool split_oversize;
   int max_parts;
   const char *channel_hint;
} provider_outbound_t;

/* =============================================================================
 * Module state — DEFINED in messaging_engine.c (core), shared across files.
 *
 * The driver registry, worker-thread handle, rate-limiter entry arrays, and
 * the Crockford alphabet stay file-local to the .c that uses them and are NOT
 * declared here.
 * ============================================================================= */

extern atomic_bool s_initialized;
extern atomic_bool s_shutdown_requested;

/* Inbound queue (bounded ring buffer) + its lock/cond. */
extern inbound_item_t *s_inbound_queue[MESSAGING_INBOUND_QUEUE_DEPTH];
extern size_t s_inbound_head;
extern size_t s_inbound_tail;
extern size_t s_inbound_count;
extern pthread_mutex_t s_inbound_mutex;
extern pthread_cond_t s_inbound_cond;

/* In-memory session map: channel row -> session_t*. */
extern session_slot_t s_session_slots[MESSAGING_MAX_SESSIONS];
extern pthread_mutex_t s_session_slots_mutex;

/* Rate limiters (entry-storage arrays are file-local to core). */
extern rate_limiter_t s_inbound_link_limiter;
extern rate_limiter_t s_inbound_general_limiter;
extern rate_limiter_t s_inbound_chat_limiter;
extern rate_limiter_t s_outbound_per_user_limiter;
extern rate_limiter_t s_read_per_user_limiter;
extern rate_limiter_t s_read_server_limiter;

/* =============================================================================
 * Cross-file helper prototypes (promoted from static when the file split).
 *
 * Generic-named helpers stay un-prefixed because they are visible only here,
 * not in the public header; the one exception is messaging_worker_thread,
 * renamed from worker_thread so it doesn't shadow worker_pool.c's static
 * symbol in a debugger/stack trace.
 * ============================================================================= */

/* Layer-boundary weak symbol — defined as a no-op in messaging_engine.c
 * (core) so the engine has no hard dependency on the WebUI layer; the strong
 * override in src/webui/webui_broadcasts.c wins when WebUI is linked.
 * Declared here (rather than pulling in the Layer-4 webui_server.h) so the
 * inbound file's process_inbound can call it without a layering violation. */
void webui_broadcast_conversation_messages_appended(int user_id, int64_t conv_id);
/* Weak no-op unless the WebUI is linked: the user's channel list changed.
 * `change` names what happened to `channel_id` (0 = several): "linked",
 * "pending" (an SMS link waiting for its code), "verified", "code_sent",
 * "code_voided", "unlinked", "enabled", "renamed", "removed".  `link_code` is
 * the (now used) link code that made a "linked"/"pending" change, else NULL,
 * so the panel closes the code it shows only when that one was used. */
void webui_broadcast_messaging_channels_changed(int user_id,
                                                int64_t channel_id,
                                                const char *change,
                                                const char *link_code);

/* core (driver registry) */
const messaging_driver_t *find_driver(const char *name);

/* core (driver registry) — first registered driver that implements the optional
 * read-history contract (list_readable_channels + read_history), or NULL.  Lets
 * the read path stay provider-neutral instead of hardcoding a driver name. */
const messaging_driver_t *find_read_capable_driver(void);

/* core (outbound delivery): render `canonical_markdown` into drv->out_format,
 * split to the provider cap (measured on converted output), and deliver each
 * part via drv->send_text with a "(N/M) " prefix + 100ms pacing.  The single
 * funnel every LLM-authored outbound reply passes through.  Does NOT mutate
 * `canonical_markdown`.  Returns MESSAGING_SUCCESS / MESSAGING_FAILURE. */
int messaging_deliver(const messaging_driver_t *drv,
                      int user_id,
                      const char *provider_address,
                      const char *address_json,
                      const char *canonical_markdown);

/* inbound (per-provider outbound shaping policy; defined in _inbound.c). */
provider_outbound_t provider_outbound_for(const char *provider);

/* channels (lookup / binding / outbound shaping helpers) */

/** What resolve_inbound_channel found. */
typedef enum {
   CHANNEL_RESOLVED = 0, /**< ref filled: this sender speaks for that channel */
   CHANNEL_NONE,         /**< no channel this sender may speak for (drop silently) */
} channel_resolve_t;

/**
 * @brief The enabled, verified channel this message's sender may speak for.
 *
 * A row is matched by (provider, address) and its owner: the row owned by
 * `sender_id`, else an unowned one, which messaging_sender_gate() then
 * decides (a one-to-one chat records this sender as its owner; a shared
 * chat with no owner needs a re-link and is logged once).  Takes the auth_db
 * lock itself.
 */
channel_resolve_t resolve_inbound_channel(const char *provider,
                                          const char *provider_address,
                                          const char *sender_id,
                                          messaging_chat_kind_t kind,
                                          channel_ref_t *ref);
/**
 * @brief The channel's forever-conversation, created on first use; also
 *        refreshes `ref->display_name` (renames show on the next turn).
 * @param gone  Optional: set true when the channel is no longer live
 *              (unlinked, or an SMS link not proven) — its messages must
 *              not be answered.
 * @return the conversation id, or 0 (no persistence this turn, or gone).
 */
int64_t resolve_channel_conversation_id(channel_ref_t *ref,
                                        const char *provider,
                                        const char *provider_address,
                                        bool *gone);
/**
 * @brief Reset a channel's forever-conversation (see messaging_engine_reset_by_name).
 * @return MESSAGING_SUCCESS / MESSAGING_UNKNOWN_CHANNEL / MESSAGING_FAILURE.
 */
int messaging_engine_reset_channel(int user_id, int64_t channel_id);
int clear_channel_conversation_id(int64_t channel_id);
/** Whether an SMS channel had an exchange within the active window. */
bool sms_within_active_window(const channel_ref_t *ref);
void touch_channel_last_used(int64_t channel_id);
void build_address_json_for(const char *provider,
                            const char *sender_address,
                            char *buf,
                            size_t buf_size);

/* codes (messaging_engine_codes.c): an action from a text waits for the
 * user's reply code (core/tool_call_challenge.h) */
struct session;
/** Whether a text is STOP or CANCEL (alone, any case). */
bool engine_is_stop_word(const char *body);
/**
 * @brief A reply code, checked once its session exists and before any turn:
 *        on the right one, the waiting action is handed over to run in the
 *        turn about to begin
 * @return true with @p out filled; false when the user was answered by text
 *         instead (a wrong code only once per action)
 */
bool engine_take_reply_code(const inbound_item_t *item, tool_redeemed_t *out);
/** A code that couldn't be taken yet (no session): the user is asked to
 *  send it again; it stays good. */
void engine_reply_busy(const inbound_item_t *item);
/**
 * @brief Run an action taken by engine_take_reply_code in the turn begun on
 *        @p session, and word the envelope that tells the model its outcome
 * @return The envelope (malloc'd); NULL on allocation failure, the user then
 *         told the outcome by text
 */
char *engine_run_reply_code(const inbound_item_t *item,
                            struct session *session,
                            tool_redeemed_t *taken);
/** STOP: drop the channel's waiting action, telling the user.  A STOP that
 *  finds nothing (a repeat that queued behind the first) gets no reply.
 *  @return whether one was dropped. */
bool engine_cancel_reply_code(const inbound_item_t *item);
/** A conversation reset by its own turn: drop what waits for a code, telling
 *  the user (the turn may have promised one). */
void engine_drop_reply_code_for_reset(const inbound_item_t *item);
/** After a turn (its reply sent): text the user the code for an action it
 *  held on the item's channel, if any, or that it expired before it could
 *  be. */
void engine_send_reply_code(const inbound_item_t *item);

/* link (async send + link-code claim) */
void engine_send_async(const messaging_driver_t *drv,
                       int user_id,
                       const char *provider_address,
                       const char *address_json,
                       const char *text);
/**
 * @brief Wait until no async send is still running.
 *
 * Each send runs on its own detached thread and uses its driver (and, when a
 * verification code fails to go, the database), so shutdown waits for them.
 *
 * @param timeout_ms How long to wait at most
 * @return SUCCESS when none is running, FAILURE if some still were at the timeout
 */
int engine_wait_async_sends(int timeout_ms);
void link_attempt_log(const char *provider,
                      const char *sender_address,
                      const char *code_tried,
                      const char *result);
/**
 * @brief `/link CODE` from a chat.  `sender_id` (the provider's id for who
 *        sent it, NULL for SMS) becomes the channel's owner; a shared chat
 *        needs one.  An SMS link waits for the code DAWN texts to the number.
 */
/** Use up a link code refused where it can't stay secret (a group chat; an
 *  SMS, which crosses the phone service and MQTT in the clear): it expires now. */
void link_code_burn(const char *code_part);
/** Whether the text after "/link " holds a code of the right shape. */
bool link_code_well_formed(const char *code_part);
int handle_link_command(const char *provider,
                        const char *sender_address,
                        const char *sender_id,
                        messaging_chat_kind_t kind,
                        const char *code_part);

/* inbound (dispatch + worker drain) */
int engine_inbound_dispatch(const messaging_inbound_t *msg);
void *messaging_worker_thread(void *arg);

/* session (slot map + staleness reload) */
void evict_session_slot(int64_t channel_id);
void slot_bump_last_known_msg_id(session_t *session, int64_t msg_id);
void reload_session_history_if_stale(session_t *session,
                                     const char *provider,
                                     const char *provider_address,
                                     int64_t conv_id,
                                     int user_id);
bool mark_pending_reset_if_self(int64_t channel_id);
session_t *get_or_create_messaging_session(const channel_ref_t *ref,
                                           const char *provider,
                                           const char *provider_address,
                                           int64_t conversation_id);

#endif /* MESSAGING_ENGINE_INTERNAL_H */
