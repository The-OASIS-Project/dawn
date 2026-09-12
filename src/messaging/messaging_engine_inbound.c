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
 * Messaging engine — inbound dispatch + worker drain.
 *
 * The driver-listener entry points (engine_inbound_dispatch + the SMS
 * variant), the /link and /new short-circuits, the injection/wake-word/rate
 * gates, the bounded inbound queue producer, the worker drain that runs the
 * LLM turn, per-provider outbound shaping + truncation, the typing-indicator
 * keepalive, and the multi-part split delivery.  Split out of
 * messaging_engine.c; see messaging_engine_internal.h and
 * docs/MESSAGING_ENGINE_SPLIT_PLAN.md.
 */
#define MESSAGING_ENGINE_INTERNAL_ALLOWED

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "core/job_dispatch.h"
#include "core/memory_filter.h"
#include "core/rate_limiter.h"
#include "core/session_manager.h"
#include "core/text_input_dispatch.h"
#include "core/wake_word.h"
#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_engine.h"
#include "messaging/messaging_engine_internal.h"
#include "messaging/messaging_split.h"

/* =============================================================================
 * Module-local constants
 * ============================================================================= */

#define MESSAGING_MAX_BODY_LEN 4096
#define MESSAGING_LINK_BODY_CAP 256
#define MESSAGING_RL_KEY_SIZE 96 /* "provider:address" composite */

/* Per-turn channel-hint buffer (the SMS system-prompt augmentation).
 * Holds the base hint plus an optional truncation-feedback append
 * when the previous reply overflowed.  See provider_outbound_for. */
/* ~640-char SMS constraints block + ~460-char truncation-feedback append
 * (~1.1 KB total) — 1024 silently truncated the prompt; 2048 leaves headroom. */
#define MESSAGING_CHANNEL_HINT_BUF_SIZE 2048

/* enqueue_inbound is referenced by engine_inbound_dispatch (below) before its
 * definition further down; the rest of the inbound surface is in source order. */
static int enqueue_inbound(const char *provider,
                           const char *provider_address,
                           const char *sender_display,
                           const char *body,
                           int64_t timestamp,
                           int user_id);

/* =============================================================================
 * Inbound dispatch (called from driver listener threads)
 * ============================================================================= */

int engine_inbound_dispatch(const char *provider,
                            const char *provider_address,
                            const char *sender_display,
                            const char *body,
                            int64_t timestamp) {
   if (!provider || !provider_address || !body) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   size_t body_len = strlen(body);

   /* Pre-DB general rate limit (per-sender).  Prevents DB-lookup DoS
    * from a stranger-flood. */
   char rl_key[MESSAGING_RL_KEY_SIZE];
   snprintf(rl_key, sizeof(rl_key), "%s:%s", provider, provider_address);
   if (rate_limiter_check(&s_inbound_general_limiter, rl_key)) {
      OLOG_DEBUG("messaging: inbound rate limit hit for %s", rl_key);
      return MESSAGING_RATE_LIMITED;
   }

   /* /link short-circuit (own stricter rate limit, body cap).  Accept
    * both "/link CODE" and "link CODE" — Slack intercepts any '/' as a
    * slash command and rejects unregistered ones, so users on that
    * provider need the slashless form.  Telegram/Discord users see
    * either form work; the slashed form stays the documented default. */
   const char *link_args = NULL;
   if (body_len >= 6 && strncmp(body, "/link ", 6) == 0) {
      link_args = body + 6;
   } else if (body_len >= 5 && strncmp(body, "link ", 5) == 0) {
      link_args = body + 5;
   }
   if (link_args) {
      if (body_len > MESSAGING_LINK_BODY_CAP) {
         OLOG_WARNING("messaging: oversized /link body from %s:%s (%zu bytes)", provider,
                      provider_address, body_len);
         link_attempt_log(provider, provider_address, NULL, "invalid");
         return MESSAGING_FAILURE;
      }
      if (rate_limiter_check(&s_inbound_link_limiter, rl_key)) {
         OLOG_WARNING("messaging: /link rate limit hit for %s:%s", provider, provider_address);
         link_attempt_log(provider, provider_address, NULL, "rate_limited");
         return MESSAGING_RATE_LIMITED;
      }
      return handle_link_command(provider, provider_address, link_args);
   }

   /* Body length cap — protects the worker queue and the LLM from
    * over-long inbound payloads.  /link is gated above with a tighter
    * 256-byte cap; everything else gets MESSAGING_MAX_BODY_LEN. */
   if (body_len > MESSAGING_MAX_BODY_LEN) {
      OLOG_WARNING("messaging: oversized inbound body from %s:%s (%zu bytes) — dropping", provider,
                   provider_address, body_len);
      return MESSAGING_FAILURE;
   }

   /* Prompt-injection filter (memory_filter_check) — matches the
    * ingestion-gate rule that already protects fetched web content
    * and memory storage paths.  Hits get dropped + warned; legitimate
    * prose isn't blocked. */
   if (memory_filter_check(body)) {
      OLOG_WARNING("messaging: inbound body from %s:%s blocked by injection filter", provider,
                   provider_address);
      return MESSAGING_FAILURE;
   }

   /* Channel lookup.  No match → silently drop (don't reply to
    * strangers; bot would be a free spam amplifier). */
   int user_id = lookup_channel_user(provider, provider_address, NULL, 0);
   if (user_id <= 0) {
      OLOG_DEBUG("messaging: inbound from unlinked %s:%s — dropping", provider, provider_address);
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   /* /new short-circuit — resets the channel's forever-conversation
    * binding.  Only honored from sender-in-channels (gated above);
    * unlinked senders can't trigger a reset on someone else's channel.
    * Recognized as case-insensitive "/new" followed by EOF or
    * whitespace; trailing content is tolerated and ignored (so "/new
    * conversation please" works as expected).  The leading '/' is
    * REQUIRED — a slashless "new" would too easily false-positive on
    * normal sentences ("New idea:", "new question:", ...).  Slack
    * users intercept '/' as a slash-command, so they reset by asking
    * the assistant to start a new conversation rather than via this
    * shortcut.  See docs/MESSAGING_CHANNELS_DESIGN.md §13 Phase 2.5. */
   if (body_len >= 4 && (body[0] == '/') && (body[1] == 'n' || body[1] == 'N') &&
       (body[2] == 'e' || body[2] == 'E') && (body[3] == 'w' || body[3] == 'W') &&
       (body_len == 4 || body[4] == ' ' || body[4] == '\t' || body[4] == '\n' || body[4] == '\r')) {
      int rc = messaging_engine_reset_channel(provider, provider_address);
      /* Send confirmation through engine_send_async so callers running
       * on the mosquitto callback thread (SMS) don't deadlock waiting
       * for the echo/response that mosquitto can't deliver while the
       * callback is blocked.  Same pattern as handle_link_command.
       * user_id was resolved above by lookup_channel_user. */
      const messaging_driver_t *drv = find_driver(provider);
      if (drv && drv->send_text) {
         char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE];
         build_address_json_for(provider, provider_address, address_json, sizeof(address_json));
         const char *msg =
             (rc == MESSAGING_SUCCESS)
                 ? "Started a new conversation. Previous context is preserved in the WebUI."
                 : "Couldn't reset the conversation (internal error). Try again or check the "
                   "WebUI.";
         engine_send_async(drv, user_id, provider_address, address_json, msg);
      }
      OLOG_INFO("messaging: /new from %s:%s (rc=%d)", provider, provider_address, rc);
      return rc;
   }

   /* Enqueue for the worker drain.  user_id is already resolved by
    * the channel lookup above — pass it through so the worker
    * doesn't have to re-query. */
   return enqueue_inbound(provider, provider_address, sender_display, body, timestamp, user_id);
}

static int enqueue_inbound(const char *provider,
                           const char *provider_address,
                           const char *sender_display,
                           const char *body,
                           int64_t timestamp,
                           int user_id) {
   pthread_mutex_lock(&s_inbound_mutex);
   if (s_inbound_count >= MESSAGING_INBOUND_QUEUE_DEPTH) {
      pthread_mutex_unlock(&s_inbound_mutex);
      OLOG_WARNING("messaging: inbound queue full, dropping message from %s:%s", provider,
                   provider_address);
      return MESSAGING_FAILURE;
   }

   inbound_item_t *item = calloc(1, sizeof(*item));
   if (!item) {
      pthread_mutex_unlock(&s_inbound_mutex);
      return MESSAGING_FAILURE;
   }
   snprintf(item->provider, sizeof(item->provider), "%s", provider);
   snprintf(item->provider_address, sizeof(item->provider_address), "%s", provider_address);
   snprintf(item->sender_display, sizeof(item->sender_display), "%s",
            sender_display ? sender_display : "");
   item->body = strdup(body);
   item->timestamp = timestamp;
   item->user_id = user_id;
   if (!item->body) {
      free(item);
      pthread_mutex_unlock(&s_inbound_mutex);
      return MESSAGING_FAILURE;
   }

   s_inbound_queue[s_inbound_tail] = item;
   s_inbound_tail = (s_inbound_tail + 1) % MESSAGING_INBOUND_QUEUE_DEPTH;
   s_inbound_count++;
   pthread_cond_signal(&s_inbound_cond);
   pthread_mutex_unlock(&s_inbound_mutex);
   return MESSAGING_SUCCESS;
}

/* =============================================================================
 * Session map and worker thread
 * ============================================================================= */

/* Per-provider outbound shaping.  The provider_outbound_t shape lives in
 * messaging_engine_internal.h (shared with messaging_deliver in core); this
 * function is the policy table.  Set the caps conservatively below the
 * documented provider ceilings, leaving 20-char headroom for the "(NN/NN) "
 * split prefix.  `channel_hint` is appended to the per-turn system prompt so
 * the LLM shapes its response to the channel. */
/* Light formatting nudge for chat channels that DO render markdown (Discord /
 * Telegram / Slack).  The formatter handles correctness deterministically;
 * this just steers the LLM away from wide tables (which flatten to fenced
 * monospace on every chat surface) at the source.  SMS keeps its own stricter
 * plain-text hint instead — appending this would contradict it. */
#define MESSAGING_CHAT_FORMAT_NUDGE                                                                \
   "[Formatting for chat delivery: prefer concise prose and short '- ' bullet lists over wide "    \
   "markdown tables — tables render poorly on chat channels.  Keep code in fenced blocks.  The " \
   "full richly-formatted version is always available in the WebUI.]"

provider_outbound_t provider_outbound_for(const char *provider) {
   if (provider && strcmp(provider, "sms") == 0) {
      /* ECHO's hard ceiling (echo/include/pdu.h:42-43):
       *   PDU_MAX_SEGMENTS       = 10
       *   PDU_UCS2_CHARS_PER_SEG = 67   (140 octets − 6 UDH ÷ 2 bytes)
       *   v1 is UCS-2-only even for ASCII bodies — see the pdu.h
       *   block comment.  So 10 × 67 = 670 chars hard cap per SEND,
       *   regardless of content.
       *
       * Using 670 BYTES (strlen) is safe because UTF-8 byte length is
       * always ≥ UCS-2 code-unit count: 1-byte ASCII → 1 UCS-2;
       * 2- or 3-byte UTF-8 → 1 UCS-2; 4-byte UTF-8 (emoji supplementary
       * plane) → 2 UCS-2.  strlen ≤ 670 guarantees the encoded UCS-2
       * fits in 10 segments.
       *
       * SMS uses split-with-cap: 670-char cap per part (one ECHO send),
       * max_parts=3 to bound segment-billing worst case for users on
       * metered plans (~30 segments worst case vs ~10 today). */
      return (provider_outbound_t){
         .max_outbound_chars = 670,
         .split_oversize = true,
         .max_parts = 3,
         .channel_hint =
             "[Delivery channel: SMS.  Your reply is being sent as a text message, NOT to a "
             "voice or web client.  HARD CONSTRAINTS for this reply, regardless of what the "
             "user asked for: "
             "(1) under 400 characters total — count them; "
             "(2) plain text only — NO markdown bold/italic, NO headers, NO bullet lists, NO "
             "emoji; "
             "(3) if the user asks for 'everything', 'all', a deep explanation, a list of "
             "items, or anything that naturally wants a long answer, give a 1-2 sentence "
             "summary and offer the WebUI for the full version (e.g. \"Quick version: X.  Want "
             "the full breakdown?  I can pull it up in the WebUI.\"). "
             "Anything you write that doesn't fit in 3 SMS messages will be dropped entirely "
             "— the user gets a short 'open the WebUI' note instead.  Keep replies short.]",
      };
   }
   if (provider && strcmp(provider, "discord") == 0) {
      /* Discord caps `content` at 2000 chars per message.  1980 leaves
       * 20-byte headroom for the optional "(NN/NN) " prefix when a
       * reply splits into multiple posts. */
      return (provider_outbound_t){
         .max_outbound_chars = 1980,
         .split_oversize = true,
         .max_parts = 0,
         .channel_hint = MESSAGING_CHAT_FORMAT_NUDGE,
      };
   }
   if (provider && strcmp(provider, "telegram") == 0) {
      /* Telegram caps `text` at 4096 chars after entities parsing.
       * 4076 leaves room for the "(NN/NN) " prefix. */
      return (provider_outbound_t){
         .max_outbound_chars = 4076,
         .split_oversize = true,
         .max_parts = 0,
         .channel_hint = MESSAGING_CHAT_FORMAT_NUDGE,
      };
   }
   if (provider && strcmp(provider, "slack") == 0) {
      /* Slack recommends ≤ 4000 chars for chat.postMessage `text`
       * (40000 hard cap, but truncation/UX degrades past 4000). */
      return (provider_outbound_t){
         .max_outbound_chars = 3980,
         .split_oversize = true,
         .max_parts = 0,
         .channel_hint = MESSAGING_CHAT_FORMAT_NUDGE,
      };
   }
   /* Unknown provider — no cap.  Driver enforces if it has its own.  This is
    * the ONLY split_oversize=false return: it pairs with the hard-truncate
    * branch in process_inbound (truncate_outbound_in_place).  No v1 provider
    * hits it (all four set split_oversize=true), but the branch is kept as a
    * documented extension point for a future hard-truncate provider. */
   return (provider_outbound_t){ .max_outbound_chars = 0,
                                 .split_oversize = false,
                                 .max_parts = 0,
                                 .channel_hint = NULL };
}

/* Truncate `text` in place to fit within `max_chars`, appending the
 * ASCII marker " ... [truncated]" at the cut.  ASCII-only so we don't
 * force UCS-2 encoding on the SMS side (which would halve segment
 * capacity).  Returns true iff a truncation happened. */
static bool truncate_outbound_in_place(char *text, size_t max_chars) {
   if (!text || max_chars == 0) {
      return false;
   }
   size_t len = strlen(text);
   if (len <= max_chars) {
      return false;
   }
   static const char marker[] = " ... [truncated]";
   const size_t marker_len = sizeof(marker) - 1;
   if (max_chars <= marker_len) {
      text[max_chars] = '\0';
      return true;
   }
   size_t cut_at = max_chars - marker_len;
   memcpy(text + cut_at, marker, marker_len);
   text[cut_at + marker_len] = '\0';
   return true;
}

/* =============================================================================
 * Typing-indicator keepalive
 * ============================================================================= */

/* Context passed to the typing-keepalive thread.  Heap-allocated by
 * process_inbound, freed after pthread_join.  Driver pointer + the
 * buffers are stable for the lifetime of the keepalive (the engine
 * worker thread holds the inbound_item_t until process_inbound
 * returns). */
typedef struct {
   const messaging_driver_t *drv;
   int user_id;
   /* Matches the inbound_item_t.provider_address sizing — chat_ids /
    * channel_ids / E.164 numbers all fit comfortably in 128. */
   char provider_address[128];
   char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE];
   atomic_bool stop;
} typing_keepalive_ctx_t;

#define MESSAGING_TYPING_INTERVAL_MS 4000
#define MESSAGING_TYPING_CHUNK_MS 100

static void *typing_keepalive_thread(void *arg) {
   typing_keepalive_ctx_t *ctx = (typing_keepalive_ctx_t *)arg;
   /* Defensive: if stop was raised between thread create and schedule,
    * skip the first call so we don't fire a typing indicator AFTER the
    * real reply has already gone out. */
   if (atomic_load(&ctx->stop)) {
      return NULL;
   }
   /* Fire once immediately so the indicator shows up even on sub-second
    * LLM turns.  Then re-fire every ~4 seconds — stays under Telegram's
    * ~5s and Discord's ~10s indicator timeouts so the icon never
    * visibly flickers off mid-turn. */
   ctx->drv->send_typing(ctx->user_id, ctx->provider_address, ctx->address_json);
   while (!atomic_load(&ctx->stop)) {
      /* 100ms-chunked sleep so the stop signal cuts in quickly when
       * the LLM call returns. */
      int chunks = MESSAGING_TYPING_INTERVAL_MS / MESSAGING_TYPING_CHUNK_MS;
      for (int i = 0; i < chunks && !atomic_load(&ctx->stop); i++) {
         struct timespec ts = { .tv_sec = 0,
                                .tv_nsec = (long)MESSAGING_TYPING_CHUNK_MS * 1000000L };
         nanosleep(&ts, NULL);
      }
      if (atomic_load(&ctx->stop)) {
         break;
      }
      ctx->drv->send_typing(ctx->user_id, ctx->provider_address, ctx->address_json);
   }
   return NULL;
}

static void process_inbound(inbound_item_t *item) {
   /* Forever-binding: every messaging-backed exchange persists into one
    * conversations row per (provider, provider_address).  First inbound
    * for a channel creates the conv; subsequent inbound reuses it.
    * /new clears the binding and the next inbound starts a fresh conv.
    * LCM handles in-place context compaction; the recovery worker
    * extracts memory incrementally via last_extracted_msg_id.
    * resolve_channel_conversation_id returns 0 on any failure — when
    * that happens we still dispatch the turn, but messages won't
    * persist to DB for this turn (degraded mode, logged at the
    * resolver).
    *
    * Resolved BEFORE get_or_create_messaging_session so the
    * conversation_id can flow into session creation — when the
    * session is freshly created (e.g., daemon just restarted), the
    * conv's full history is restored into session->conversation_history
    * so the LLM picks up where the user left off. */
   int64_t conv_id = resolve_channel_conversation_id(item->provider, item->provider_address,
                                                     item->user_id);

   session_t *session = get_or_create_messaging_session(item->provider, item->provider_address,
                                                        item->user_id, conv_id);
   if (!session) {
      OLOG_ERROR("messaging: failed to acquire session for %s:%s", item->provider,
                 item->provider_address);
      return;
   }

   /* Cross-channel staleness check.  An external writer (WebUI
    * conversation panel, voice session, MCP) may have appended to
    * this conv between our turns — the cached session_t holds frozen
    * in-memory history while the DB has fresher state.  Without this
    * reload, the LLM would respond without context of the
    * external-channel turns, producing a visibly-divergent
    * conversation from what the user sees in the WebUI.
    *
    * Universal across SMS / Telegram / future Discord/Slack — every
    * channel funnels through here.  Skipped when conv_id <= 0 (DB
    * persistence failed for this turn; degraded mode). */
   reload_session_history_if_stale(session, item->provider, item->provider_address, conv_id,
                                   item->user_id);

   /* Build the per-turn channel hint, including a truncation-feedback
    * note when the prior assistant reply was cut.  The truncation
    * marker lives in the assistant history (we replaced it
    * post-truncation in the previous turn), so detecting prior
    * truncation is just a substring check — no separate per-session
    * "was-truncated" state needed. */
   provider_outbound_t cfg = provider_outbound_for(item->provider);
   char channel_hint_buf[MESSAGING_CHANNEL_HINT_BUF_SIZE] = { 0 };
   const char *channel_hint = cfg.channel_hint;
   if (channel_hint) {
      char *last_assistant = session_get_last_message_content(session, "assistant");
      /* Anchor the truncation-detect on the exact marker the
       * truncator emits at the END of an oversized message (see
       * truncate_outbound_in_place).  Suffix-match (not strstr) so
       * user content containing the literal "[truncated]" mid-text
       * doesn't trip the guardrail. */
      static const char trunc_marker[] = " ... [truncated]";
      bool prev_truncated = false;
      if (last_assistant) {
         size_t la_len = strlen(last_assistant);
         size_t mk_len = sizeof(trunc_marker) - 1;
         if (la_len >= mk_len && strcmp(last_assistant + la_len - mk_len, trunc_marker) == 0) {
            prev_truncated = true;
         }
      }
      free(last_assistant);
      if (prev_truncated) {
         snprintf(channel_hint_buf, sizeof(channel_hint_buf),
                  "%s  IMPORTANT: your previous reply OVERFLOWED and was truncated "
                  "mid-sentence — the user got only the text before the [truncated] marker "
                  "visible in your last assistant message above.  This is the failure mode "
                  "the constraints above are meant to prevent.  Your reply this turn MUST be "
                  "1-2 sentences max, even if it means deferring detail to the WebUI.",
                  channel_hint);
         channel_hint = channel_hint_buf;
      }
   }

   text_input_dispatch_opts_t opts = {
      .conversation_id = conv_id,
      .auth_user_id = item->user_id,
      .sentence_cb = NULL,
      .sentence_userdata = NULL,
      .on_user_msg_added = NULL,
      .user_msg_added_ctx = NULL,
      .channel_hint = channel_hint,
   };

   /* Typing-indicator keepalive.  If the driver supports `send_typing`,
    * spawn a background thread that fires the typing call once and
    * re-fires every ~4s while the LLM is processing — so the user sees
    * "Bot is typing..." instead of dead air during multi-second turns.
    * Joined after dispatch returns; ctx freed here (not in the thread)
    * for clear ownership. */
   typing_keepalive_ctx_t *ka_ctx = NULL;
   pthread_t ka_thread = 0;
   bool ka_started = false;
   const messaging_driver_t *typing_drv = find_driver(item->provider);
   if (typing_drv && typing_drv->send_typing) {
      ka_ctx = (typing_keepalive_ctx_t *)calloc(1, sizeof(*ka_ctx));
      if (ka_ctx) {
         ka_ctx->drv = typing_drv;
         ka_ctx->user_id = item->user_id;
         snprintf(ka_ctx->provider_address, sizeof(ka_ctx->provider_address), "%s",
                  item->provider_address);
         build_address_json_for(item->provider, item->provider_address, ka_ctx->address_json,
                                sizeof(ka_ctx->address_json));
         atomic_init(&ka_ctx->stop, false);
         if (pthread_create(&ka_thread, NULL, typing_keepalive_thread, ka_ctx) == 0) {
            ka_started = true;
         } else {
            free(ka_ctx);
            ka_ctx = NULL;
         }
      }
   }

   /* Persist the tool-turn's assistant tool_calls + role:tool rows to the channel
    * conversation for the whole (synchronous) dispatch — the SAME mechanism jobs use
    * (job_dispatch_tool_persist_cb, snapshot conv_id).  A messaging turn is otherwise on
    * the tool loop's fan_ephemeral path with no persist hook, so without this a WebUI
    * viewer reloading the channel conversation loses the tool interaction (the chat
    * platform only ever receives the final answer, which persists inline below).  pctx is
    * stack-scoped and the hook fires on THIS thread during dispatch; cleared right after. */
   job_persist_ctx_t pctx = { conv_id, item->user_id };
   if (conv_id > 0) {
      session_set_tool_persist_hook(session, job_dispatch_tool_persist_cb, &pctx);
   }
   char *response = core_text_input_dispatch(session, item->body, NULL, NULL, NULL, 0, &opts);
   session_set_tool_persist_hook(session, NULL, NULL);

   /* Stop the typing keepalive before doing anything else with the
    * response — including the empty-response early return below, so
    * the keepalive doesn't continue firing after we've already given
    * up on this turn. */
   if (ka_started) {
      atomic_store(&ka_ctx->stop, true);
      pthread_join(ka_thread, NULL);
      free(ka_ctx);
      ka_ctx = NULL;
   }

   if (!response || response[0] == '\0') {
      session_release(session);
      free(response);
      return;
   }

   /* Provider-side truncation safety net for any provider that opts
    * into hard-truncate semantics (split_oversize=false).  No v1
    * provider uses this path — all four are split_oversize=true and
    * fall through to the split block at the driver-send site below.
    * Kept available for any future provider where hard-truncate is
    * the right failure mode.
    *
    * History-vs-delivery state on this turn:
    *   (A) Hard-truncate (HERE): history rewritten to the truncated
    *       text — user got exactly that; LLM next turn sees what was
    *       sent and the prev_truncated cue above kicks in.
    *   (B) Multi-part split (driver-send block below): history holds
    *       the FULL response — split is a transport detail, user
    *       received the whole thing across N posts.
    *   (C) Split rejected (also driver-send block): history holds
    *       the FULL response (the LLM's actual output); user got
    *       only the err_msg pointing them at the WebUI. */
   if (cfg.max_outbound_chars > 0 && !cfg.split_oversize) {
      size_t original_len = strlen(response);
      if (truncate_outbound_in_place(response, cfg.max_outbound_chars)) {
         OLOG_WARNING("messaging: truncated %s outbound %zu→%zu chars for %s:%s", item->provider,
                      original_len, strlen(response), item->provider, item->provider_address);
         if (!session_replace_last_message_content(session, "assistant", response)) {
            /* Either the assistant message uses multi-part content
             * (vision) or history was mutated under us — log only. */
            OLOG_DEBUG("messaging: couldn't replace assistant history with truncated text");
         }
      }
   }

   /* Persist the assistant message to conv_db so the WebUI conversation
    * list shows the full thread, the recovery worker can extract
    * memory incrementally, and LCM context_expand can drill back to
    * original text after compaction.  We persist the POST-truncation
    * text — that's what the user actually received, and what the
    * in-memory history now holds.  conv_db_add_message_ex enforces
    * ownership; passing user_id is mandatory.  Failure is logged but
    * non-fatal: the turn already happened in memory and the response
    * still goes to the driver. */
   if (conv_id > 0) {
      int64_t assistant_msg_id = 0;
      int rc = conv_db_add_message_ex(conv_id, item->user_id, "assistant", response,
                                      &assistant_msg_id);
      if (rc == AUTH_DB_SUCCESS && assistant_msg_id > 0) {
         session_stamp_last_message_id(session, "assistant", assistant_msg_id);
         /* Bump slot's last_known_msg_id to the assistant message.
          * msg_ids are monotonic per conv, so this id is strictly
          * greater than the just-persisted user message's id — one
          * bump covers both writes for the cross-channel staleness
          * check on the next inbound. */
         slot_bump_last_known_msg_id(session, assistant_msg_id);
         /* Notify any open WebUI session viewing this conversation
          * that new messages landed.  Weak-symbol no-op when the
          * WebUI layer isn't linked; the strong override in
          * webui_broadcasts.c filters to the channel's owning user
          * and pushes a `conversation_messages_appended` JSON event
          * to all of their sessions.  The client gates on
          * activeConversationId === conv_id and re-fetches.  Fires
          * AFTER persistence completes — single broadcast per turn
          * (the user message was also written, but the WebUI's
          * reload picks up both in one fetch).  Universal across
          * SMS / Telegram / future Discord/Slack since every
          * channel funnels through process_inbound. */
         webui_broadcast_conversation_messages_appended(item->user_id, conv_id);
      } else if (rc != AUTH_DB_SUCCESS) {
         OLOG_WARNING("messaging: failed to persist assistant message to conv %lld (rc=%d)",
                      (long long)conv_id, rc);
      }
   }

   /* Touch last_used_at so the SMS active-conversation window slides
    * forward.  Also drives outbound rate-limit accounting and the
    * "most-recent channel" sort in any future WebUI surfacing.  Cheap
    * UPDATE; failure is silent (not enough signal to log). */
   touch_channel_last_used(item->provider, item->provider_address);

   session_release(session);

   /* Send the response back via the originating driver.  Pass
    * provider_address natively so the driver can skip the JSON parse
    * on the hot path.  Builder still constructs the address_json blob
    * for providers that need extras.  user_id flows from the inbound
    * item so drivers that scope per-user state (SMS audit log + rate
    * buckets in phone_service) accumulate against the correct DAWN
    * user, not a hardcoded admin fallback.
    *
    * Split path: when the response exceeds the provider's per-message
    * cap and split_oversize=true, carve the text at natural break
    * points (paragraphs → sentences → reject) and deliver as multiple
    * posts with 100ms pacing.  When the splitter rejects (no break in
    * window) or the engine rejects (parts > max_parts), deliver the
    * err_msg as a single short message — user sees "open the WebUI"
    * rather than silent loss.  See messaging_split.h for the
    * algorithm; see state machine A/B/C above the truncate block for
    * how delivery interacts with conversation history. */
   const messaging_driver_t *drv = find_driver(item->provider);
   if (drv && drv->send_text) {
      char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE];
      build_address_json_for(item->provider, item->provider_address, address_json,
                             sizeof(address_json));

      if (cfg.split_oversize) {
         /* Render the canonical markdown into the driver's native format
          * (SMS plain / Telegram HTML / Discord / Slack mrkdwn), split it to
          * the provider cap measured on the CONVERTED output, and deliver
          * each part with the (N/M) prefix.  `response` is passed read-only —
          * the stored conversation copy stays canonical CommonMark for the
          * WebUI.  No raw-length pre-gate: an expanding conversion (e.g.
          * Discord table→fence, Slack &→&amp;) could cross the cap only after
          * conversion, so the decision lives inside the formatter. */
         messaging_deliver(drv, item->user_id, item->provider_address, address_json, response);
      } else {
         /* Hard-truncate providers (none in v1): `response` was already
          * truncated in place above; send it as a single raw message. */
         drv->send_text(item->user_id, item->provider_address, address_json, response);
      }
   }
   free(response);

   /* Deferred self-reset processing.  When the LLM's tool call
    * targeted this very session for `/new`, mark_pending_reset_if_self
    * set the flag instead of triggering an immediate reset (which
    * would have UAF'd this worker thread).  Now that the response is
    * sent and our retain on `session` is dropped, it's safe to
    * actually reset.  Clear the flag under the slots mutex
    * (concurrent worker access is impossible — single worker thread —
    * but defensive against future multi-worker refactors), then
    * perform the standard reset_channel path. */
   bool do_deferred_reset = false;
   pthread_mutex_lock(&s_session_slots_mutex);
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session && strcmp(s_session_slots[i].provider, item->provider) == 0 &&
          strcmp(s_session_slots[i].provider_address, item->provider_address) == 0 &&
          s_session_slots[i].pending_reset) {
         s_session_slots[i].pending_reset = false;
         do_deferred_reset = true;
         break;
      }
   }
   pthread_mutex_unlock(&s_session_slots_mutex);

   if (do_deferred_reset) {
      OLOG_INFO("messaging: processing deferred self-reset for %s:%s", item->provider,
                item->provider_address);
      evict_session_slot(item->provider, item->provider_address);
      clear_channel_conversation_id(item->provider, item->provider_address);
   }
}

void *messaging_worker_thread(void *arg) {
   (void)arg;
   OLOG_INFO("messaging: worker thread started");

   while (!atomic_load(&s_shutdown_requested)) {
      pthread_mutex_lock(&s_inbound_mutex);
      while (s_inbound_count == 0 && !atomic_load(&s_shutdown_requested)) {
         pthread_cond_wait(&s_inbound_cond, &s_inbound_mutex);
      }
      if (atomic_load(&s_shutdown_requested)) {
         pthread_mutex_unlock(&s_inbound_mutex);
         break;
      }
      inbound_item_t *item = s_inbound_queue[s_inbound_head];
      s_inbound_queue[s_inbound_head] = NULL;
      s_inbound_head = (s_inbound_head + 1) % MESSAGING_INBOUND_QUEUE_DEPTH;
      s_inbound_count--;
      pthread_mutex_unlock(&s_inbound_mutex);

      if (item) {
         process_inbound(item);
         free(item->body);
         free(item);
      }
   }

   OLOG_INFO("messaging: worker thread exiting");
   return NULL;
}

/* =============================================================================
 * SMS inbound entry point (called by phone_service.c on echo/events)
 * ============================================================================= */

int messaging_engine_handle_sms_inbound(const char *sender_e164,
                                        const char *sender_display,
                                        const char *body,
                                        int64_t timestamp) {
   if (!sender_e164 || sender_e164[0] == '\0' || !body) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   size_t body_len = strlen(body);

   /* Pre-DB general inbound rate limit, same shape as the Telegram
    * dispatcher.  Prevents a flood of texts to the modem from
    * hammering the DB lookup path. */
   char rl_key[MESSAGING_RL_KEY_SIZE];
   snprintf(rl_key, sizeof(rl_key), "sms:%s", sender_e164);
   if (rate_limiter_check(&s_inbound_general_limiter, rl_key)) {
      OLOG_DEBUG("messaging: SMS inbound rate limit hit for %s", sender_e164);
      return MESSAGING_RATE_LIMITED;
   }

   /* /link short-circuit.  This runs BEFORE the wake-word gate
    * because the linking flow is the only way a sender's E.164 ever
    * gets into messaging_channels in the first place — gating /link
    * on "must be linked already" would lock everyone out. */
   if (body_len >= 6 && strncmp(body, "/link ", 6) == 0) {
      if (body_len > MESSAGING_LINK_BODY_CAP) {
         OLOG_WARNING("messaging: oversized SMS /link body from %s (%zu bytes)", sender_e164,
                      body_len);
         link_attempt_log("sms", sender_e164, NULL, "invalid");
         return MESSAGING_FAILURE;
      }
      if (rate_limiter_check(&s_inbound_link_limiter, rl_key)) {
         OLOG_WARNING("messaging: SMS /link rate limit hit for %s", sender_e164);
         link_attempt_log("sms", sender_e164, NULL, "rate_limited");
         return MESSAGING_RATE_LIMITED;
      }
      return handle_link_command("sms", sender_e164, body + 6);
   }

   /* Body length cap (same shape as Telegram path). */
   if (body_len > MESSAGING_MAX_BODY_LEN) {
      OLOG_WARNING("messaging: oversized SMS inbound body from %s (%zu bytes) — falling through",
                   sender_e164, body_len);
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   /* Injection filter — same gate the Telegram path uses, and the same
    * gate that protects every other LLM-ingestion path in DAWN. */
   if (memory_filter_check(body)) {
      OLOG_WARNING("messaging: SMS inbound from %s blocked by injection filter — falling through",
                   sender_e164);
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   /* /new short-circuit — runs BEFORE the wake-word gate so users
    * don't have to say "Hey Friday /new", but AFTER the injection
    * filter and channel-lookup so unlinked senders can't trigger
    * resets.  Same recognition shape as the Telegram path: case-
    * insensitive "/new" + EOF or whitespace; trailing content
    * ignored.  See docs/MESSAGING_CHANNELS_DESIGN.md §13 Phase 2.5. */
   if (body_len >= 4 && (body[0] == '/') && (body[1] == 'n' || body[1] == 'N') &&
       (body[2] == 'e' || body[2] == 'E') && (body[3] == 'w' || body[3] == 'W') &&
       (body_len == 4 || body[4] == ' ' || body[4] == '\t' || body[4] == '\n' || body[4] == '\r')) {
      /* Sender-in-channels guard.  Unlinked phones can't reset
       * someone else's binding.  Capture user_id for the
       * confirmation-send so the driver scopes its audit + rate
       * buckets against the right user. */
      int reset_user_id = lookup_channel_user("sms", sender_e164, NULL, 0);
      if (reset_user_id <= 0) {
         OLOG_DEBUG("messaging: SMS /new from unlinked %s — dropping", sender_e164);
         return MESSAGING_UNKNOWN_CHANNEL;
      }
      int rc = messaging_engine_reset_channel("sms", sender_e164);
      const messaging_driver_t *drv = find_driver("sms");
      if (drv && drv->send_text) {
         char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE];
         build_address_json_for("sms", sender_e164, address_json, sizeof(address_json));
         const char *msg =
             (rc == MESSAGING_SUCCESS)
                 ? "Started a new conversation. Previous context is preserved in the WebUI."
                 : "Couldn't reset the conversation (internal error). Try again or check the "
                   "WebUI.";
         engine_send_async(drv, reset_user_id, sender_e164, address_json, msg);
      }
      OLOG_INFO("messaging: /new from sms:%s (rc=%d)", sender_e164, rc);
      return MESSAGING_SUCCESS;
   }

   /* Active-conversation window — skip the wake-word gate when this
    * sender's channel had an LLM-bound exchange recently.  Mirrors the
    * iMessage thread metaphor: once you're in a back-and-forth with
    * Friday, you don't need to re-announce her name on every reply.
    * The window slides forward on each successful exchange (see
    * touch_channel_last_used in process_inbound).  Telegram/Discord/
    * Slack don't need this (LLM-exclusive — every linked-sender
    * message routes to LLM unconditionally). */
   bool active_window = sms_within_active_window(sender_e164);
   const char *cmd = NULL;
   if (active_window) {
      /* No wake-word required — route the full body as the user
       * command.  Sender-in-channels was implicitly verified by the
       * window check (window only returns true for linked channels). */
      cmd = body;
   } else {
      /* Wake-word prefix gate.  Use the start-anchored matcher
       * (wake_word_check_prefix); the existing wake_word_check() uses
       * strstr substring search which is exploitable on
       * arbitrary-sender text. */
      wake_word_result_t wr = wake_word_check_prefix(body);
      if (!wr.detected) {
         return MESSAGING_UNKNOWN_CHANNEL; /* caller falls through */
      }
      /* Pass just the command remainder to the LLM, not the wake
       * word.  If the user only said the wake word with no command
       * ("hey friday"), there's nothing for the LLM to act on. */
      cmd = (wr.has_command && wr.command) ? wr.command : "";
   }

   /* Sender-in-channels check.  Even with a perfect wake-word prefix
    * (or an open active-window match), an unlinked phone number
    * can't reach the LLM. */
   int user_id = lookup_channel_user("sms", sender_e164, NULL, 0);
   if (user_id <= 0) {
      OLOG_DEBUG("messaging: SMS from unlinked %s passed wake-word but no channel — dropping",
                 sender_e164);
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   if (cmd[0] == '\0') {
      return MESSAGING_SUCCESS; /* handled — no-op */
   }

   return enqueue_inbound("sms", sender_e164, sender_display ? sender_display : "", cmd, timestamp,
                          user_id);
}
