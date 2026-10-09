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
 * Messaging driver contract (Layer 3, with the engine).
 *
 * Each chat-app provider (Telegram, Discord, Slack) and the existing SMS
 * path implements this contract.  The engine
 * (src/messaging/messaging_engine.c) registers drivers at init, owns the
 * per-user channel resolution, and routes inbound + outbound messages
 * through this interface.  See docs/MESSAGING_CHANNELS_DESIGN.md §4.
 */
#ifndef MESSAGING_DRIVER_H
#define MESSAGING_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "messaging/messaging_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The longest one driver send may take (seconds): the HTTP timeout of a
 *  Telegram, Discord or Slack message.  Shutdown waits past it for sends in
 *  flight (MESSAGING_ASYNC_SEND_DRAIN_MS). */
#define MESSAGING_SEND_TIMEOUT_SEC 30L

/** Whether a chat holds one person (with the bot) or several. */
typedef enum {
   MESSAGING_CHAT_ONE_TO_ONE = 0,
   MESSAGING_CHAT_SHARED = 1,
} messaging_chat_kind_t;

/**
 * @brief One inbound message, as a driver hands it to the engine.
 *
 * A channel is linked to the person who linked it, not to the chat: the
 * engine answers a message only when `sender_id` is that person.  So
 * `sender_id` must come from the provider's own record of who sent the
 * message (Telegram `from.id`, Discord `author.id`, Slack `event.user`),
 * never from text the message carries.  A driver drops what has no single,
 * provider-known person behind it (anonymous group admins, posts in a chat's
 * name, bots, forwarded or inline-bot content).  Any later way of approving
 * something from a chat (a button press, say) must go through the same check.
 */
typedef struct {
   const char *provider;         /**< Driver name ("telegram" / "discord" / ...) */
   const char *provider_address; /**< The chat: chat_id / channel_id / phone */
   const char *sender_id;        /**< Provider id of the person; NULL if unknown */
   const char *sender_display;   /**< Human-readable sender name (may be NULL) */
   const char *body;             /**< UTF-8 text */
   int64_t timestamp;            /**< Unix seconds the provider claims (0 = unknown) */
   messaging_chat_kind_t chat_kind;
} messaging_inbound_t;

/**
 * @brief Callback invoked by a driver when an inbound message arrives.
 *
 * The driver runs this on its listener thread (do NOT block — the
 * engine's bound queue absorbs back-pressure on the worker drain).
 *
 * @return SUCCESS if the engine accepted the event, FAILURE if it
 *         couldn't enqueue (queue full, body too long, gate rejected,
 *         etc.).  Drivers MAY ignore the return — the engine's
 *         response semantics are downstream of enqueue.
 */
typedef int (*messaging_inbound_fn)(const messaging_inbound_t *msg);

/**
 * @brief Time/cursor window for a read_history() fetch.
 *
 * Bundles the per-fetch bounds so the contract doesn't grow a flat parameter
 * list as new bounds are added.  All wall-clock fields are Unix seconds.
 *   - after_ts  : lower bound (0 = no lower bound).
 *   - before_ts : upper bound (0 = up to now).
 *   - before_id : exact older-history cursor (a provider message id); when
 *                 non-NULL/non-empty it pages strictly older than that message,
 *                 overriding before_ts.  NULL/"" = use before_ts.
 *   - limit     : desired max messages; the driver clamps to its own cap.
 */
typedef struct {
   int64_t after_ts;
   int64_t before_ts;
   const char *before_id;
   int limit;
} messaging_read_window_t;

/**
 * @brief Per-driver function table.
 *
 * Drivers own persistent connections (long-poll loops, Gateway
 * WebSockets, Socket Mode WebSockets) — unlike `embedding_provider_t`
 * (stateless request/response).  The connection-state hooks reflect
 * that.
 */
typedef struct messaging_driver_s {
   /** Driver name — used as the `provider` column value
    *  ("telegram" / "discord" / "slack" / "sms"). */
   const char *name;

   /** The provider vouches for who sent each message: a chat app's sender id
    *  comes from the provider's own record (true for Telegram, Discord,
    *  Slack).  False for SMS: the sender number can be forged, so a channel
    *  can't be owned by a sender and a new link is proven by a code texted
    *  to the number. */
   bool authenticates_sender;

   /** Wire format this driver consumes.  The engine renders every outbound
    *  message into this dialect (via messaging_deliver / engine_send_async)
    *  BEFORE calling send_text(), so the driver may statically rely on
    *  receiving its own format — e.g. the Telegram driver hardcodes
    *  parse_mode=HTML.  See include/messaging/messaging_format.h. */
   messaging_format_t out_format;

   /**
    * Initialize the driver.  Spawns the listener thread, opens the
    * persistent connection, etc.  Returns SUCCESS / FAILURE.  The
    * `credentials_json` blob is provider-specific (e.g.,
    * `{"bot_token": "..."}` for Telegram).
    *
    * Called once at engine init time.
    */
   int (*init)(const char *credentials_json);

   /**
    * Tear the driver down.  Closes the connection, joins the listener
    * thread.  Called at engine shutdown.
    */
   void (*shutdown)(void);

   /**
    * Send a plain-text message to a provider address.
    *
    * @param user_id           The DAWN user the send is acting on
    *                          behalf of.  Drivers that scope per-user
    *                          state (SMS audit logs, rate-limit
    *                          buckets in phone_service) consume this;
    *                          drivers that don't (Telegram bot tokens
    *                          are bot-wide) ignore it.  Must be > 0.
    * @param provider_address  Typed primary key for this driver
    *                          (chat_id / phone_e164 / channel_id).
    *                          Drivers that need ONLY this can skip
    *                          parsing `address_json`.  Must be
    *                          non-NULL.
    * @param address_json      Full address blob with provider extras
    *                          (e.g. Discord's guild_id, Slack's
    *                          team_id, Telegram's reply_to overrides).
    *                          May be NULL or "{}" for providers with
    *                          no extras (SMS, Telegram MVP).  Driver
    *                          parses only when it needs an extra.
    * @param text              UTF-8 message body.  Driver may segment
    *                          if provider has a per-message length cap.
    *
    * @return SUCCESS / FAILURE.  Network errors map to FAILURE; the
    *         engine layer may retry per its rate-limit policy.
    */
   int (*send_text)(int user_id,
                    const char *provider_address,
                    const char *address_json,
                    const char *text);

   /**
    * OPTIONAL — send a code text (a link or reply code), whose content must
    * not be kept.  A driver that records what it sends records `log_text`
    * instead.  It must not be dropped by the driver's own send limits (its
    * callers cap codes themselves): a dropped code strands what waits for it.
    * Drivers that keep no copy may leave this NULL; send_text is used for a
    * link code.  A reply code is sent only through this hook (it fails closed
    * without it): a driver whose senders it can't verify must provide it.
    */
   int (*send_text_unlogged)(int user_id,
                             const char *provider_address,
                             const char *address_json,
                             const char *text,
                             const char *log_text);

   /**
    * Build the canonical address_json blob for this driver given a
    * provider_address.  Used by the engine when it has only a typed
    * primary key (e.g., the inbound dispatcher's process_inbound,
    * /link confirmations, /new replies) and needs to construct the
    * full JSON shape for downstream consumers.  Each driver owns the
    * shape of its own JSON — eliminates the engine's strcmp-on-provider
    * switch and lets Phase 3 (Discord) add a new driver without
    * touching the engine.
    *
    * Drivers SHOULD write `{}` (or the minimal valid shape) when
    * `provider_address` is the only data available.  Buffer too small
    * → caller's responsibility; drivers SHOULD null-terminate within
    * `buf_size`.
    *
    * @param provider_address  Typed primary key.
    * @param buf               Caller-provided buffer.
    * @param buf_size          Buffer size (recommended >= 256).
    */
   void (*build_address_json)(const char *provider_address, char *buf, size_t buf_size);

   /**
    * Register the inbound-event sink.  Called once at engine init,
    * before init().  The driver stores the pointer and invokes it for
    * every inbound message.
    *
    * @return SUCCESS / FAILURE.
    */
   int (*register_inbound_cb)(messaging_inbound_fn cb);

   /**
    * Validate that `address_json` has the shape this driver expects
    * BEFORE INSERTing it into messaging_channels.  Catches malformed
    * JSON, missing keys, out-of-range integers, bad E.164, etc.
    *
    * @return SUCCESS if address is well-formed, FAILURE otherwise.
    */
   int (*validate_address)(const char *address_json);

   /**
    * @return 1 if the driver's persistent connection is currently
    *         healthy, 0 otherwise.  Engine uses this for health-check
    *         endpoints; the driver itself handles automatic reconnect.
    */
   int (*is_connected)(void);

   /**
    * Engine may request an explicit reconnect (e.g., after a config
    * change).  The driver tears down and re-establishes the connection.
    *
    * @return SUCCESS / FAILURE.  FAILURE means the driver couldn't
    *         reconnect; engine should mark the driver disabled.
    */
   int (*reconnect)(void);

   /**
    * OPTIONAL — fire a "typing" indicator to the recipient so they see
    * "Bot is typing..." while the engine processes the inbound message.
    * Fire-and-forget: failures (network, throttling) are NOT propagated.
    *
    * Engine spawns a keepalive thread for the lifetime of LLM
    * processing and re-fires this every ~4 seconds (stays under
    * Telegram's ~5s + Discord's ~10s indicator timeouts).
    *
    * Drivers without a "typing" concept (SMS, future Slack via bot
    * tokens) leave this NULL.
    *
    * @param user_id           DAWN user the typing is on behalf of.
    *                          Drivers with bot-wide tokens may ignore.
    * @param provider_address  Typed primary key (chat_id /
    *                          channel_id).  Must be non-NULL/empty.
    * @param address_json      Full address blob — drivers that need
    *                          extras consult this.  May be NULL or
    *                          "{}" otherwise.
    */
   void (*send_typing)(int user_id, const char *provider_address, const char *address_json);

   /**
    * OPTIONAL — enumerate the channels this driver/bot can read history from.
    *
    * Used by the read-channel path to fuzzy-match a user-named channel to a
    * provider channel id.  Writes a heap-allocated, provider-NEUTRAL JSON
    * array into *out_json (caller frees):
    *
    *   [{"container_id":"...","container_name":"...",
    *     "channel_id":"...","channel_name":"...","type":<int>}, ...]
    *
    * "container" abstracts the grouping a provider uses — guild (Discord),
    * workspace (future Slack).  Drivers MAY cache internally (the engine does
    * not cache the parsed result).  Note: enumeration reflects channels the
    * bot *may* be able to read; per-channel read permission is only known on
    * the actual read_history() call.
    *
    * Drivers that cannot read history (telegram/sms/slack-v1) leave NULL.
    *
    * @return SUCCESS / FAILURE (never a count).
    */
   int (*list_readable_channels)(char **out_json);

   /**
    * OPTIONAL — fetch the MOST-RECENT up-to-`window->limit` messages in
    * `channel_id` within `window` (see messaging_read_window_t).  Returns them
    * newest-first as the provider naturally orders them.  Writes a
    * heap-allocated JSON array into *out_json (caller frees):
    *
    *   [{"id":"...","author":"...","timestamp":<unix_secs>,
    *     "content":"...","type":<int>,"is_bot":<0|1>}, ...]
    *
    * The driver maps the window bounds to whatever cursors its API uses
    * (Discord: synthetic snowflakes for after/before) so the contract stays
    * provider-neutral.  The driver clamps `window->limit` to its own provider
    * cap and bounds pagination.  Missing read permission may surface as an
    * empty array rather than an error, depending on the provider — callers must
    * treat empty as "nothing to show, possibly no permission".
    *
    * Drivers that cannot read history leave NULL.
    *
    * @return SUCCESS / FAILURE (never a count).
    */
   int (*read_history)(const char *channel_id,
                       const messaging_read_window_t *window,
                       char **out_json);

   /**
    * OPTIONAL — drop any cached result of list_readable_channels() so the next
    * call refetches.  Lets the engine recover when a fuzzy name-resolution miss
    * is caused by a channel created within the discovery cache TTL.  NULL for
    * drivers without a discovery cache.
    */
   void (*invalidate_readable_channels_cache)(void);
} messaging_driver_t;

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_DRIVER_H */
