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
 * Messaging LLM tool — actions: list_channels / send / read_channel /
 * read_server / list_discord_channels / link_status / reset_conversation.
 * Delegates to messaging_engine.  See docs/MESSAGING_CHANNELS_DESIGN.md §3.
 */
#include "tools/messaging_tool.h"

#include <json-c/json.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/scheduled_context.h"
#include "core/session_manager.h"
#include "core/time_query_parser.h"
#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_discord.h"
#include "messaging/messaging_engine.h"
#include "messaging/messaging_slack.h"
#include "messaging/messaging_sms.h"
#include "messaging/messaging_telegram.h"
#include "tools/tool_registry.h"

/* Mirror of DC_SNOWFLAKE_MAX_DIGITS (messaging_discord_internal.h, private to the
 * Discord translation units) — the tool layer can't include that header, so keep
 * the two in sync. */
#define MSG_TOOL_SNOWFLAKE_MAX_DIGITS 20

static char *make_response(const char *msg) {
   return msg ? strdup(msg) : NULL;
}

static char *handle_list_channels(int user_id) {
   char *json = messaging_engine_list_channels_json(user_id);
   if (!json) {
      return make_response(TOOL_RESULT_ERROR_MARK "Error: could not list channels.");
   }
   /* Wrap the JSON in a brief sentence so the LLM has natural framing. */
   size_t needed = strlen(json) + 64;
   char *result = malloc(needed);
   if (!result) {
      free(json);
      return NULL;
   }
   snprintf(result, needed, "Linked channels: %s", json);
   free(json);
   return result;
}

static char *handle_send(struct json_object *details, int user_id) {
   if (!details) {
      return make_response("Error: 'send' requires details with 'channel' and 'text'.");
   }

   struct json_object *chan_obj = NULL;
   struct json_object *text_obj = NULL;
   if (!json_object_object_get_ex(details, "channel", &chan_obj) ||
       !json_object_object_get_ex(details, "text", &text_obj)) {
      return make_response("Error: 'send' requires 'channel' and 'text' fields.");
   }
   const char *channel = json_object_get_string(chan_obj);
   const char *text = json_object_get_string(text_obj);
   if (!channel || !text || text[0] == '\0') {
      return make_response("Error: 'channel' and 'text' must be non-empty.");
   }

   int rc = messaging_engine_send(user_id, channel, text);
   switch (rc) {
      case MESSAGING_SUCCESS:
         return make_response("Message sent.");
      case MESSAGING_UNKNOWN_CHANNEL:
         return make_response(
             TOOL_RESULT_ERROR_MARK
             "Error: no channel by that name is linked. Use action 'list_channels' to see what's "
             "available, or have the user generate a linking code in the WebUI.");
      case MESSAGING_RATE_LIMITED:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: rate limit hit for this channel (default 10/min, 200/day). "
                              "Wait and retry, or surface this to the user.");
      case MESSAGING_PROVIDER_RATE_LIMITED:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: the provider's own rate limit was hit. Retry shortly.");
      case MESSAGING_DRIVER_NOT_REGISTERED:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: the driver for that channel's provider isn't configured. "
                              "Check secrets.toml and dawn.toml [messaging] section.");
      default:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: send failed (network or provider error).");
   }
}

static char *handle_reset_conversation(struct json_object *details, int user_id) {
   if (!details) {
      return make_response("Error: 'reset_conversation' requires details with a 'channel' field.");
   }
   struct json_object *chan_obj = NULL;
   if (!json_object_object_get_ex(details, "channel", &chan_obj)) {
      return make_response("Error: 'reset_conversation' requires 'channel'.");
   }
   const char *channel = json_object_get_string(chan_obj);
   if (!channel || channel[0] == '\0') {
      return make_response("Error: 'channel' must be non-empty.");
   }
   int rc = messaging_engine_reset_by_name(user_id, channel);
   switch (rc) {
      case MESSAGING_SUCCESS:
         return make_response("Conversation reset. The next message on that channel will start a "
                              "fresh thread; prior history is preserved in the WebUI.");
      case MESSAGING_UNKNOWN_CHANNEL:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: no channel by that name is linked. Use 'list_channels' to "
                              "see what's available.");
      default:
         return make_response(TOOL_RESULT_ERROR_MARK "Error: reset failed (internal error).");
   }
}

/* Parse an optional natural-language time phrase from `details[key]` into a
 * Unix-seconds bound.  `upper` selects the parsed window's END (until/before)
 * vs its START (since/after).  Returns 0 when the field is absent or
 * unparseable (the engine treats 0 as "unbounded" on that end). */
static int64_t read_time_bound(struct json_object *details, const char *key, bool upper) {
   struct json_object *obj = NULL;
   if (!details || !json_object_object_get_ex(details, key, &obj)) {
      return 0;
   }
   const char *phrase = json_object_get_string(obj);
   if (!phrase || !phrase[0]) {
      return 0;
   }
   time_query_t tq;
   if (time_query_parse(phrase, (int64_t)time(NULL), &tq) != SUCCESS || !tq.found) {
      return 0;
   }
   /* target_ts is the period's reference point and window_seconds its half-width.
    * For a lower bound use the reference point itself (so "last week" → ~7d ago,
    * not ~14d); for an upper bound use the far (later) edge so the named day/period
    * is fully included (e.g. "until 2026-06-07" reaches the end of the 7th). */
   int64_t bound = upper ? (tq.target_ts + tq.window_seconds) : tq.target_ts;
   return bound > 0 ? bound : 0;
}

static char *handle_read_channel(struct json_object *details, int user_id) {
   if (!details) {
      return make_response("Error: 'read_channel' requires details with a 'channel' field.");
   }
   struct json_object *chan_obj = NULL;
   if (!json_object_object_get_ex(details, "channel", &chan_obj)) {
      return make_response("Error: 'read_channel' requires 'channel'.");
   }
   const char *channel = json_object_get_string(chan_obj);
   if (!channel || channel[0] == '\0') {
      return make_response("Error: 'channel' must be non-empty.");
   }

   /* Optional time range: 'since' (lower) and 'until' (upper); 0 = unbounded. */
   int64_t since_ts = read_time_bound(details, "since", false);
   int64_t until_ts = read_time_bound(details, "until", true);

   int limit = 0; /* engine applies the default + cap */
   struct json_object *limit_obj = NULL;
   if (json_object_object_get_ex(details, "limit", &limit_obj)) {
      limit = json_object_get_int(limit_obj);
   }
   const char *server = NULL;
   struct json_object *srv_obj = NULL;
   if (json_object_object_get_ex(details, "server", &srv_obj)) {
      server = json_object_get_string(srv_obj);
   }
   /* Optional 'before' older-history cursor: a message id (snowflake).  Validate
    * digits-only AND length here so a malformed cursor returns a clear error
    * instead of being silently rejected by the driver as a generic read failure. */
   const char *before_id = NULL;
   struct json_object *before_obj = NULL;
   if (json_object_object_get_ex(details, "before", &before_obj)) {
      const char *b = json_object_get_string(before_obj);
      if (b && b[0]) {
         size_t blen = 0;
         for (const char *p = b; *p; p++, blen++) {
            if (*p < '0' || *p > '9') {
               return make_response(
                   TOOL_RESULT_ERROR_MARK
                   "Error: 'before' must be a numeric Discord message id (digits only).");
            }
         }
         if (blen > MSG_TOOL_SNOWFLAKE_MAX_DIGITS) {
            return make_response(TOOL_RESULT_ERROR_MARK
                                 "Error: 'before' is too long to be a Discord message id (max "
                                 "20 digits).");
         }
         before_id = b;
      }
   }

   char *out = NULL;
   const messaging_read_channel_opts_t opts = {
      .channel_name = channel,
      .since_ts = since_ts,
      .until_ts = until_ts,
      .before_id = before_id,
      .limit = limit,
      .server_hint = server,
   };
   int rc = messaging_engine_read_channel(user_id, &opts, &out);
   switch (rc) {
      case MESSAGING_SUCCESS:
         return out ? out : make_response("(no content)");
      case MESSAGING_UNKNOWN_CHANNEL:
         return make_response(
             TOOL_RESULT_ERROR_MARK
             "Error: I can't see a channel by that name. The bot must be invited to the server, "
             "and I only read text/announcement channels. Try the exact channel name, or include "
             "the server name (e.g. add a 'server' field) if the name exists in multiple servers.");
      case MESSAGING_RATE_LIMITED:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: too many channel reads recently. Wait a bit and retry.");
      case MESSAGING_DRIVER_NOT_REGISTERED:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: channel reading isn't available — it requires a configured "
                              "Discord bot (reading is Discord-only).");
      default:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: couldn't read that channel (network or provider error).");
   }
}

/* Upper bound on the explicit channel-subset list a single read_server may
 * carry.  Deliberately >= the engine's per-sweep read cap
 * (MSG_READ_SERVER_MAX_CHANNELS, 30): this only bounds how many NAMES the
 * request may list; the engine still reads at most 30 and reports the true
 * "covered N of M" denominator. */
#define MSG_TOOL_READ_SERVER_CHANNELS_MAX 40

static char *handle_read_server(struct json_object *details, int user_id) {
   /* All optional: {server, since, until, channels:["general","announcements"]}. */
   const char *server = NULL;
   struct json_object *srv_obj = NULL;
   if (details && json_object_object_get_ex(details, "server", &srv_obj)) {
      server = json_object_get_string(srv_obj);
   }
   int64_t since_ts = read_time_bound(details, "since", false);
   int64_t until_ts = read_time_bound(details, "until", true);

   /* Optional explicit channel subset.  Pointers borrow from `details`, which
    * outlives this call (freed by the dispatcher after we return). */
   const char *channels[MSG_TOOL_READ_SERVER_CHANNELS_MAX];
   int channel_count = 0;
   struct json_object *chans_obj = NULL;
   if (details && json_object_object_get_ex(details, "channels", &chans_obj) &&
       json_object_is_type(chans_obj, json_type_array)) {
      int m = (int)json_object_array_length(chans_obj);
      for (int i = 0; i < m && channel_count < MSG_TOOL_READ_SERVER_CHANNELS_MAX; i++) {
         const char *s = json_object_get_string(json_object_array_get_idx(chans_obj, i));
         if (s && s[0]) {
            channels[channel_count++] = s;
         }
      }
   }

   char *out = NULL;
   const messaging_read_server_opts_t opts = {
      .server_hint = server,
      .since_ts = since_ts,
      .until_ts = until_ts,
      .channels = channel_count ? channels : NULL,
      .channel_count = channel_count,
   };
   int rc = messaging_engine_read_server(user_id, &opts, &out);
   switch (rc) {
      case MESSAGING_SUCCESS:
         return out ? out : make_response("(no content)");
      case MESSAGING_UNKNOWN_CHANNEL:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: I'm not in any Discord server I can read. Invite the bot to "
                              "the server (with View Channels + Read Message History).");
      case MESSAGING_RATE_LIMITED:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: too many channel reads recently. Wait a bit and retry.");
      case MESSAGING_DRIVER_NOT_REGISTERED:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: channel reading isn't available — it requires a configured "
                              "Discord bot (reading is Discord-only).");
      default:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: couldn't read the server (network or provider error).");
   }
}

static char *handle_list_discord_channels(struct json_object *details, int user_id) {
   /* Optional {server: 'My Server'} filter. */
   const char *server = NULL;
   struct json_object *srv_obj = NULL;
   if (details && json_object_object_get_ex(details, "server", &srv_obj)) {
      server = json_object_get_string(srv_obj);
   }
   char *out = NULL;
   int rc = messaging_engine_list_discord_channels(user_id, server, &out);
   switch (rc) {
      case MESSAGING_SUCCESS:
         return out ? out : make_response("(no content)");
      case MESSAGING_RATE_LIMITED:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: too many channel reads recently. Wait a bit and retry.");
      case MESSAGING_DRIVER_NOT_REGISTERED:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: channel listing isn't available — it requires a configured "
                              "Discord bot (Discord-only).");
      default:
         return make_response(TOOL_RESULT_ERROR_MARK
                              "Error: couldn't list channels (network or provider error).");
   }
}

static char *handle_link_status(struct json_object *details) {
   if (!details) {
      return make_response("Error: 'link_status' requires a 'code' field.");
   }
   struct json_object *code_obj = NULL;
   if (!json_object_object_get_ex(details, "code", &code_obj)) {
      return make_response("Error: 'link_status' requires 'code'.");
   }
   const char *code = json_object_get_string(code_obj);
   if (!code) {
      return make_response(TOOL_RESULT_ERROR_MARK "Error: invalid code.");
   }

   messaging_link_state_t state = messaging_engine_link_status(code);
   switch (state) {
      case MESSAGING_LINK_STATE_PENDING:
         return make_response("Link code is pending — user has not yet sent /link to a bot.");
      case MESSAGING_LINK_STATE_CLAIMED:
         return make_response("Link code has been claimed. Channel is now active.");
      case MESSAGING_LINK_STATE_EXPIRED:
         return make_response(
             "Link code has expired. Generate a new one in WebUI Settings → Messaging.");
      case MESSAGING_LINK_STATE_NOT_FOUND:
      default:
         return make_response(TOOL_RESULT_ERROR_MARK "Link code not recognized.");
   }
}

/* Single source of truth for which messaging actions may run unattended (from a
 * scheduled task or briefing).  Only read-only Discord actions qualify; send and
 * channel management require a live conversation (a human in the loop).  Used by
 * BOTH the fire-time gate in messaging_callback and the create-time gate
 * (messaging_validate_schedulable_action, reached via tool_registry). */
static bool messaging_action_is_schedulable(const char *action) {
   return action && (strcmp(action, "read_channel") == 0 || strcmp(action, "read_server") == 0 ||
                     strcmp(action, "list_discord_channels") == 0);
}

#define MESSAGING_SCHEDULABLE_ERR                                                         \
   "only read-only Discord actions (read_channel / read_server / list_discord_channels) " \
   "may run from a schedule; other messaging actions (send, etc.) require a live conversation."

/* Per-action schedulability gate registered in messaging_metadata.  Rejects
 * non-read actions at scheduler CREATE time so the LLM is told up front rather
 * than silently failing at fire time.  Mirrors the fire-time gate below. */
static int messaging_validate_schedulable_action(const char *action,
                                                 char *err_buf,
                                                 size_t err_buf_size) {
   if (messaging_action_is_schedulable(action)) {
      return SUCCESS;
   }
   if (err_buf && err_buf_size) {
      snprintf(err_buf, err_buf_size, MESSAGING_SCHEDULABLE_ERR);
   }
   return FAILURE;
}

static char *messaging_callback(const char *action, char *value, int *should_respond) {
   if (should_respond) {
      *should_respond = 1;
   }
   if (!action) {
      return make_response("Error: missing action.");
   }

   /* Resolve user_id.  Interactive turns carry a thread-local session
    * (command context); scheduled briefing steps run on the scheduler thread
    * with no session, so fall back to the scheduled-origin context the
    * briefing executor sets — otherwise this would silently bill/audit reads
    * to user 1.  See include/core/scheduled_context.h. */
   int user_id = 1;
   int sched_user = 0;
   bool is_scheduled = scheduled_context_get(&sched_user);
   session_t *ctx = session_get_command_context();
   if (ctx && ctx->metrics.user_id > 0) {
      user_id = ctx->metrics.user_id;
   } else if (is_scheduled && sched_user > 0) {
      user_id = sched_user;
   }

   /* Fire-time action-level schedulability gate: the tool carries
    * TOOL_CAP_SCHEDULABLE (so the read-digest use case works), but only
    * read-only actions may run unattended.  Reject everything else when invoked
    * from a scheduled context — keyed on is_scheduled, NOT "no session", since
    * the identity fallback above sets a context-equivalent for scheduled runs.
    * Defense in depth: the same verdict is enforced at scheduler create time via
    * messaging_validate_schedulable_action(), so a non-read action should never
    * reach a schedule — but legacy rows created before this gate still fire here.
    * Shares messaging_action_is_schedulable() as the single allowlist. */
   if (is_scheduled && !messaging_action_is_schedulable(action)) {
      return make_response(TOOL_RESULT_ERROR_MARK "Error: " MESSAGING_SCHEDULABLE_ERR);
   }

   /* Parse details JSON if present. */
   struct json_object *details = NULL;
   if (value && value[0] != '\0') {
      details = json_tokener_parse(value);
   }

   char *result = NULL;
   if (strcmp(action, "list_channels") == 0) {
      result = handle_list_channels(user_id);
   } else if (strcmp(action, "send") == 0) {
      result = handle_send(details, user_id);
   } else if (strcmp(action, "read_channel") == 0) {
      result = handle_read_channel(details, user_id);
   } else if (strcmp(action, "read_server") == 0) {
      result = handle_read_server(details, user_id);
   } else if (strcmp(action, "list_discord_channels") == 0) {
      result = handle_list_discord_channels(details, user_id);
   } else if (strcmp(action, "link_status") == 0) {
      result = handle_link_status(details);
   } else if (strcmp(action, "reset_conversation") == 0) {
      result = handle_reset_conversation(details, user_id);
   } else {
      char buf[128];
      snprintf(buf, sizeof(buf), TOOL_RESULT_ERROR_MARK "Error: unknown action '%s'.", action);
      result = make_response(buf);
   }

   if (details) {
      json_object_put(details);
   }
   return result;
}

static const treg_param_t messaging_params[] = {
   {
       .name = "action",
       .description = "The messaging action: 'list_channels' (show linked channels for the "
                      "current user), 'send' (deliver a text message to a named channel — use "
                      "ONLY to reach a channel OTHER than the one you are currently chatting on, "
                      "or to message proactively when not in a chat; to answer the channel you "
                      "are already talking on, just reply normally — your reply is delivered "
                      "there automatically, and 'send'-ing to it would duplicate your reply), "
                      "'read_channel' (Discord only — read recent messages from a server channel "
                      "the bot can see, e.g. 'catch me up on #general', and summarize them in "
                      "your reply), "
                      "'read_server' (Discord only — read EVERY readable channel of one server "
                      "and summarize each, e.g. 'sum up everything on my server'; bounded to the "
                      "most recent messages per channel), "
                      "'list_discord_channels' (Discord only — cheaply list the channels the bot "
                      "can see WITHOUT reading any messages; use this first to discover the server "
                      "layout before deciding what to read), "
                      "'link_status' (check whether a pending link code has been claimed), "
                      "'reset_conversation' (close the current forever-thread on a channel and "
                      "start fresh next message; prior history is preserved in the WebUI)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "list_channels", "send", "read_channel", "read_server",
                        "list_discord_channels", "link_status", "reset_conversation" },
       .enum_count = 7,
   },
   {
       .name = "details",
       .description =
           "JSON object with action-specific fields (pass as JSON-encoded string).\n"
           "For 'send': {channel: 'telegram_main', text: 'message body'}.\n"
           "For 'read_channel': {channel: 'general', since: 'last week', until: 'yesterday', "
           "limit: 100, server: 'My Server'}. 'channel' is the Discord channel name (with or "
           "without '#'). 'since'/'until' bound a time range — natural phrases ('today', 'this "
           "morning', '2 hours ago', 'last week', 'last month', 'yesterday') or ISO dates "
           "('2026-06-01'). Give 'since' alone for everything from then to now, both for a closed "
           "range, or neither for the most recent messages. 'limit' is an optional max message "
           "count (default 100, hard cap 300); 'server' is optional and disambiguates a channel "
           "name that exists in multiple servers. 'before' is an optional message id to page "
           "further back in history — the transcript ends with the oldest message id, which you "
           "pass as 'before' on the next call to read older messages. The bot reads any "
           "text/announcement channel it has been invited to — NOT restricted to channels you "
           "linked. Returns a transcript to summarize; if the name is ambiguous it returns the "
           "matching servers to pick from.\n"
           "For 'read_server': {server: 'My Server', since: 'last week', until: 'yesterday', "
           "channels: ['general','announcements']} (all optional). Reads readable channels of one "
           "Discord server and returns one transcript with a section per channel — summarize each. "
           "'server' picks which server (omit if the bot is in only one); 'since'/'until' bound a "
           "time range as for 'read_channel'; 'channels' restricts to a named subset (use it to "
           "read just a few channels, or to fetch the remaining channels after a truncated sweep — "
           "pair with 'list_discord_channels' to get the names). Bounded to the most-recent "
           "messages per channel; quiet channels are marked '(no recent activity)'.\n"
           "For 'list_discord_channels': {server: 'My Server'} (optional). Lists the channels the "
           "bot can see grouped by server, WITHOUT reading any messages — cheap discovery so you "
           "know the layout before choosing what to 'read_channel'.\n"
           "For 'link_status': {code: 'DAWNA7K9PQ'}.\n"
           "For 'reset_conversation': {channel: 'telegram_main'} — equivalent to the user "
           "sending /new in the chat app.\n"
           "For 'list_channels': no fields required.\n"
           "  channel: the display_name shown by 'list_channels' — must match EXACTLY. "
           "Cannot be invented; if no channel by that name appears in 'list_channels' "
           "output, the user has not linked one. Tell the user to link via WebUI Settings.\n"
           "  text: message body. SMS-linked channels truncate to ~400 chars on outbound "
           "(engine-enforced); Telegram/Discord allow longer. Keep concise for SMS.\n"
           "  code: 8-character uppercase Crockford base32 (e.g. 'DAWNA7K9PQ'); generated "
           "via the WebUI Settings → Messaging panel.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* Which optional drivers we've registered, so messaging_tool_refresh_drivers()
 * can start a newly-tokened driver live (WebUI "Save Secrets") without trying to
 * re-register one that's already running.  SMS is unconditional and not tracked. */
static bool s_telegram_registered = false;
static bool s_discord_registered = false;
static bool s_slack_registered = false;
/* Guards the flags + the register calls.  In practice the two callers don't
 * overlap (init runs before the WebUI accepts connections; the live refresh
 * runs on the lws service thread afterward), but the mutex closes the
 * theoretical race and documents the shared state. */
static pthread_mutex_t s_driver_reg_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Register the optional token-gated drivers whose tokens are present and which
 * aren't already registered.  Additive only — never tears a running driver down
 * (token rotation/removal is daemon-restart-to-apply; live teardown would join a
 * long-poll listener and the driver isn't cleanly unregisterable from the engine
 * today).  Called at init and again from set_secrets so adding a token in the
 * WebUI starts the driver without a restart. */
static void register_token_drivers(void) {
   pthread_mutex_lock(&s_driver_reg_mutex);
   if (!s_telegram_registered && g_secrets.telegram_bot_token[0] != '\0') {
      if (messaging_telegram_register(g_secrets.telegram_bot_token) == SUCCESS) {
         s_telegram_registered = true;
      } else {
         OLOG_WARNING("messaging_tool: Telegram driver registration failed");
      }
   }
   if (!s_discord_registered && g_secrets.discord_bot_token[0] != '\0') {
      if (messaging_discord_register(g_secrets.discord_bot_token) == SUCCESS) {
         s_discord_registered = true;
      } else {
         OLOG_WARNING("messaging_tool: Discord driver registration failed");
      }
   }
   if (!s_slack_registered && g_secrets.slack_app_token[0] != '\0' &&
       g_secrets.slack_bot_token[0] != '\0') {
      if (messaging_slack_register(g_secrets.slack_app_token, g_secrets.slack_bot_token) ==
          SUCCESS) {
         s_slack_registered = true;
      } else {
         OLOG_WARNING("messaging_tool: Slack driver registration failed");
      }
   }
   pthread_mutex_unlock(&s_driver_reg_mutex);
}

void messaging_tool_refresh_drivers(void) {
   register_token_drivers();
}

static int messaging_tool_init(void) {
   if (messaging_engine_init() != MESSAGING_SUCCESS) {
      OLOG_ERROR("messaging_tool: engine init failed");
      return FAILURE;
   }
   /* SMS driver always registers — modem connection lifecycle belongs
    * to ECHO, not the messaging engine.  When the modem is offline,
    * outbound send_text fails gracefully through phone_service. */
   if (messaging_sms_register() != SUCCESS) {
      OLOG_WARNING("messaging_tool: SMS driver registration failed");
   }

   /* Conditionally register drivers based on configured tokens.  When no token
    * is set, the engine still runs (the LLM-facing tool can report "no channels
    * linked"), but no driver listens.  Adding a token later via the WebUI starts
    * the driver live (messaging_tool_refresh_drivers); removing/rotating one is
    * daemon-restart-to-apply. */
   if (g_secrets.telegram_bot_token[0] == '\0') {
      OLOG_INFO("messaging_tool: no telegram_bot_token configured; Telegram disabled");
   }
   if (g_secrets.discord_bot_token[0] == '\0') {
      OLOG_INFO("messaging_tool: no discord_bot_token configured; Discord disabled");
   }
   if (g_secrets.slack_app_token[0] == '\0' || g_secrets.slack_bot_token[0] == '\0') {
      OLOG_INFO("messaging_tool: no slack_app_token/slack_bot_token configured; Slack disabled");
   }
   register_token_drivers();
   return SUCCESS;
}

static void messaging_tool_cleanup(void) {
   if (g_secrets.slack_app_token[0] != '\0' && g_secrets.slack_bot_token[0] != '\0') {
      messaging_slack_shutdown();
   }
   if (g_secrets.discord_bot_token[0] != '\0') {
      messaging_discord_shutdown();
   }
   if (g_secrets.telegram_bot_token[0] != '\0') {
      messaging_telegram_shutdown();
   }
   messaging_sms_shutdown();
   messaging_engine_shutdown();
}

static const tool_metadata_t messaging_metadata = {
   .name = "messaging",
   .device_string = "messaging",
   .topic = "dawn",
   .aliases = { "message", "send_message", "chat" },
   .alias_count = 3,

   .description = "Send, read, and manage messages across linked chat platforms (Telegram, "
                  "Discord, Slack) and SMS. Use 'list_channels' to see what's linked, "
                  "'send' to deliver text to a named channel, 'link_status' to check "
                  "pending link codes. Discord-only reading: 'read_channel' / 'read_server' "
                  "summarize history from any channel the bot can see (fuzzy-matched by name), "
                  "'list_discord_channels' lists them. IMPORTANT: when you are already conversing "
                  "on a messaging channel, do NOT use 'send' to reply to that same channel — "
                  "just answer normally and your reply is delivered there. Reserve 'send' "
                  "for reaching a DIFFERENT channel or for proactive/unprompted messages. "
                  "Note that 'send' targets only WebUI-LINKED channels (a different set from the "
                  "bot-visible channels you can read). SCHEDULING: only the read actions "
                  "(read_channel / read_server / list_discord_channels) may run from the scheduler "
                  "or a briefing; 'send', 'reset_conversation', and 'link_status' require a live "
                  "conversation and are rejected if scheduled — do not offer to schedule them. "
                  "Each user manages their own channels via the WebUI Settings panel.",
   .params = messaging_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NETWORK | TOOL_CAP_SCHEDULABLE,
   .default_local = true,
   .default_remote = true,

   .validate_schedulable_action = messaging_validate_schedulable_action,

   .init = messaging_tool_init,
   .cleanup = messaging_tool_cleanup,
   .callback = messaging_callback,
};

int messaging_tool_register(void) {
   return tool_registry_register(&messaging_metadata);
}
