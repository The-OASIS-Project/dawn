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
 * Rebuilding a conversation's LLM context from the database: the one canonical
 * restore used by sidebar load, session-expiry recovery, and a turn whose
 * conversation isn't the one the session holds.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_interface.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "memory/memory_history_loader.h"
#include "utils/string_utils.h"
#include "webui/webui_handlers.h"
#include "webui/webui_image_rehydrate.h"
#include "webui/webui_internal.h"

/* Skip leading ASCII whitespace; returns the first non-whitespace char. */
static const char *restore_skip_ws(const char *s) {
   while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
      s++;
   }
   return s;
}

/* If @p s begins with "<dawn:" then (optional whitespace) @p kw, return a pointer just past
 * @p kw; else NULL.  Whitespace-tolerant so a malformed imitated marker ("<dawn: reasoning")
 * still matches. */
static const char *restore_match_dawn_open(const char *s, const char *kw) {
   static const char prefix[] = "<dawn:";
   if (strncmp(s, prefix, sizeof(prefix) - 1) != 0) {
      return NULL;
   }
   const char *p = restore_skip_ws(s + sizeof(prefix) - 1);
   size_t klen = strlen(kw);
   return (strncmp(p, kw, klen) == 0) ? p + klen : NULL;
}

/* Find the position just past the next "</dawn:" (ws?) "thinking" (ws?) ">" close tag, or
 * NULL if none.  Whitespace-tolerant to match the imitated-marker variants. */
static const char *restore_find_dawn_close_thinking(const char *s) {
   for (const char *c = strstr(s, "</dawn:"); c; c = strstr(c + 1, "</dawn:")) {
      const char *p = restore_skip_ws(c + 7); /* strlen("</dawn:") */
      if (strncmp(p, "thinking", 8) != 0) {
         continue;
      }
      p = restore_skip_ws(p + 8);
      if (*p == '>') {
         return p + 1;
      }
   }
   return NULL;
}

/*
 * Strip ONLY leading legacy display markers from assistant content before it enters the
 * session's LLM-facing history.  The pre-E3 client persistence path prepended
 * "<dawn:reasoning .../>" and "<dawn:thinking ...>...</dawn:thinking>" blocks to assistant
 * content; they are a DISPLAY artifact (the reload path extracts them into a panel) and must
 * never reach the LLM — a reasoning model restored onto such a conversation imitates the
 * marker format in its own output (observed 2026-06-04 after a provider switch, where the
 * model emitted a fabricated "<dawn: reasoning tokens=...">).
 *
 * Scope is deliberately tight: only the LEADING marker block(s) (the legacy prepend
 * position), and the caller applies this to ASSISTANT messages only.  A mid-message mention
 * of these tags (e.g. discussing DAWN's code) is left untouched.  Returns a newly-allocated
 * cleaned copy, or NULL if nothing was stripped (caller keeps the original pointer).
 */
static char *restore_strip_leading_dawn_markers(const char *content) {
   if (!content) {
      return NULL;
   }
   const char *p = content;
   for (;;) {
      const char *q = restore_skip_ws(p);
      if (restore_match_dawn_open(q, "reasoning")) {
         /* Self-closing tag: advance past its '>'. */
         const char *gt = strchr(q, '>');
         if (!gt) {
            break; /* malformed/unterminated — stop, keep the remainder intact */
         }
         p = gt + 1;
         continue;
      }
      if (restore_match_dawn_open(q, "thinking")) {
         const char *close = restore_find_dawn_close_thinking(q);
         if (!close) {
            break; /* unterminated block — stop, don't eat the real answer */
         }
         p = close;
         continue;
      }
      break;
   }
   if (p == content) {
      return NULL; /* no leading markers */
   }
   return strdup(restore_skip_ws(p)); /* trim the blank line before the real answer */
}

/**
 * @brief Message callback for session context restoration.
 * Builds JSON objects with role + content for iterating into session_add_message.
 */
static int webui_session_restore_msg_cb(const conversation_llm_row_t *msg, void *context) {
   json_object *arr = (json_object *)context;
   json_object *obj = json_object_new_object();
   /* The row id rides along into the context so memory extraction can verify
    * which conversation every message came from. */
   json_object_object_add(obj, "id", json_object_new_int64(msg->id));
   json_object_object_add(obj, "role", json_object_new_string(msg->role));
   json_object_object_add(obj, "content", json_object_new_string(msg->content ? msg->content : ""));
   /* Carry structured tool fields so the restore loop can rebuild OpenAI-canonical
    * tool messages (assistant tool_calls / role:tool) for the LLM (E2). */
   if (msg->tool_calls && msg->tool_calls[0]) {
      json_object *tc = json_tokener_parse(msg->tool_calls);
      if (tc) {
         json_object_object_add(obj, "tool_calls", tc);
      }
   }
   if (msg->tool_call_id && msg->tool_call_id[0]) {
      json_object_object_add(obj, "tool_call_id", json_object_new_string(msg->tool_call_id));
   }
   json_object_array_add(arr, obj);
   return 0;
}

/* Add @p src's DB row id (if any) to @p dst. */
static void copy_row_id(json_object *src, json_object *dst) {
   json_object *id_obj;
   if (dst && json_object_object_get_ex(src, "id", &id_obj) && json_object_get_int64(id_obj) > 0) {
      json_object_object_add(dst, "id", json_object_new_int64(json_object_get_int64(id_obj)));
   }
}

/**
 * @brief Build a conversation's LLM context from DB, detached from any session
 *
 * The one canonical restore (compaction watermark + summary, image rehydration,
 * tool-message rebuild), shared by sidebar load, session-expiry recovery, and a
 * turn whose conversation is not the one loaded (session_turn_begin's loader).
 * Every restored message carries its DB row id.
 *
 * @param full_system_prompt Build the user's system prompt when the stored
 *                       messages carry none; otherwise a placeholder (a turn
 *                       rebuilds its system messages before calling the LLM)
 * @param has_system_out Receives whether the stored messages carried their own
 *                       system prompt (otherwise one is added)
 * @return New history array (caller owns), or NULL (DB read failed / OOM)
 */
static json_object *webui_build_conversation_context(int user_id,
                                                     const conversation_t *conv,
                                                     int64_t conv_id,
                                                     bool full_system_prompt,
                                                     bool *has_system_out,
                                                     int *count_out) {
   *has_system_out = false;
   *count_out = 0;
   json_object *all_msgs = json_object_new_array();
   if (!all_msgs) {
      return NULL;
   }
   /* v67: bound restored context to messages after the compaction watermark;
    * the injected summary (below) stands in for the compacted prefix. The
    * full transcript is still shown in the UI (display load is unbounded).
    * Each assistant turn comes back with its stored blocks, so it replays as
    * the model produced it. */
   const int rc = memory_history_load_rows(conv_id, user_id, conv->context_watermark_msg_id, true,
                                           webui_session_restore_msg_cb, all_msgs, all_msgs, NULL);
   if (rc != AUTH_DB_SUCCESS) {
      json_object_put(all_msgs);
      return NULL;
   }

   json_object *hist = json_object_new_array();
   if (!hist) {
      json_object_put(all_msgs);
      return NULL;
   }
   int count = json_object_array_length(all_msgs);

   /* Check if stored messages include a system prompt */
   bool has_system = false;
   if (count > 0) {
      json_object *first = json_object_array_get_idx(all_msgs, 0);
      json_object *role_obj;
      if (json_object_object_get_ex(first, "role", &role_obj)) {
         const char *role = json_object_get_string(role_obj);
         if (role && strcmp(role, "system") == 0)
            has_system = true;
      }
   }

   if (!has_system) {
      char *prompt = full_system_prompt ? session_manager_build_system_prompt_string(user_id)
                                        : NULL;
      json_object *sys = json_object_new_object();
      if (sys) {
         json_object_object_add(sys, "role", json_object_new_string("system"));
         json_object_object_add(
             sys, "content", json_object_new_string(prompt ? prompt : get_remote_command_prompt()));
         json_object_array_add(hist, sys);
      }
      free(prompt);
   }

   if (conv->compaction_summary && strlen(conv->compaction_summary) > 0) {
      /* v67: prepend a reconstructed [COMPACTED ...] marker (when a summary node
       * exists) so the reloaded LLM keeps a context_expand handle to the
       * compacted originals — not just the summary text.
       *
       * ASSISTANT role, NOT system: session_update_system_messages rebuilds the
       * leading context into exactly two system messages (stable prefix + volatile
       * focus block) every turn and DROPS any other system message — so a
       * system-role summary never reaches the LLM. The live compaction marker
       * (llm_context.c) is an assistant message for the same reason; matching it
       * here makes the summary survive the per-turn rebuild. */
      char summary[CONV_SUMMARY_MAX];
      conv_db_format_compaction_context(conv_id, conv->compaction_summary, summary,
                                        sizeof(summary));
      json_object *m = json_object_new_object();
      if (m) {
         json_object_object_add(m, "role", json_object_new_string("assistant"));
         json_object_object_add(m, "content", json_object_new_string(summary));
         json_object_array_add(hist, m);
      }
   }

   for (int i = 0; i < count; i++) {
      json_object *msg = json_object_array_get_idx(all_msgs, i);
      json_object *role_obj, *content_obj;
      if (json_object_object_get_ex(msg, "role", &role_obj) &&
          json_object_object_get_ex(msg, "content", &content_obj)) {
         const char *role = json_object_get_string(role_obj);
         const char *content = json_object_get_string(content_obj);

         /* Strip leading legacy <dawn:reasoning/>/<dawn:thinking> display markers from
          * assistant content so the LLM never sees them (and stops imitating the format).
          * Assistant-only, leading-only — see restore_strip_leading_dawn_markers. */
         char *stripped_content = NULL;
         if (role && strcmp(role, "assistant") == 0) {
            stripped_content = restore_strip_leading_dawn_markers(content);
            if (stripped_content) {
               content = stripped_content;
            }
         }

         json_object *m = NULL;
         json_object *tc_obj, *tcid_obj;
         bool has_tc = json_object_object_get_ex(msg, "tool_calls", &tc_obj);
         bool has_tcid = json_object_object_get_ex(msg, "tool_call_id", &tcid_obj);
         if (has_tc || has_tcid) {
            /* Rebuild the OpenAI-canonical tool message in-memory so the LLM sees the
             * structured call/result on reload.  Orphans (a tool result whose call
             * didn't restore, etc.) are dropped later by filter_orphaned_tool_messages
             * at request-build time. */
            m = json_object_new_object();
            if (m) {
               json_object_object_add(m, "role", json_object_new_string(role));
               json_object_object_add(m, "content", json_object_new_string(content ? content : ""));
               if (has_tc) {
                  json_object_object_add(m, "tool_calls", json_object_get(tc_obj));
               }
               if (has_tcid) {
                  json_object_object_add(m, "tool_call_id", json_object_get(tcid_obj));
               }
            }
         } else {
            /* Rehydrate [IMAGE:img_id] markers into LLM-faithful image_url content
             * (owner-checked); no-marker messages become plain text. */
            m = webui_rehydrate_message(user_id, role, content ? content : "");
         }
         if (m) {
            copy_row_id(msg, m);
            json_object *blocks = NULL;
            if (json_object_object_get_ex(msg, LLM_TURN_BLOCKS_KEY, &blocks)) {
               json_object_object_add(m, LLM_TURN_BLOCKS_KEY, json_object_get(blocks));
            }
            json_object_array_add(hist, m);
         }
         free(stripped_content);
      }
   }

   json_object_put(all_msgs);
   *has_system_out = has_system;
   *count_out = count;
   return hist;
}

/* A conversation's stored LLM settings (its last-used model/provider/thinking)
 * applied over @p base into @p out.  false when it stores none. */
static bool conversation_llm_config(const conversation_t *conv,
                                    const session_llm_config_t *base,
                                    session_llm_config_t *out) {
   /* tools_mode is a retired dead column — no longer hydrated. */
   if (conv->llm_type[0] == '\0') {
      return false;
   }
   session_llm_config_t cfg = *base;

   if (strcmp(conv->llm_type, "local") == 0)
      cfg.type = LLM_LOCAL;
   else if (strcmp(conv->llm_type, "cloud") == 0)
      cfg.type = LLM_CLOUD;
   if (conv->cloud_provider[0] != '\0') {
      if (strcmp(conv->cloud_provider, "openai") == 0)
         cfg.cloud_provider = CLOUD_PROVIDER_OPENAI;
      else if (strcmp(conv->cloud_provider, "claude") == 0)
         cfg.cloud_provider = CLOUD_PROVIDER_CLAUDE;
      else if (strcmp(conv->cloud_provider, "gemini") == 0)
         cfg.cloud_provider = CLOUD_PROVIDER_GEMINI;
      else if (strcmp(conv->cloud_provider, "openrouter") == 0)
         cfg.cloud_provider = CLOUD_PROVIDER_OPENROUTER;
   }
   if (conv->model[0] != '\0') {
      safe_strscpy(cfg.model, conv->model);

      /* Infer provider from model name if not explicitly stored. OpenRouter IDs are
       * "vendor/model" slugs and match none of the bare prefixes below, so an
       * OpenRouter conversation's provider/model are left as stored. */
      if (conv->cloud_provider[0] == '\0') {
         if (strncmp(conv->model, "gpt-", 4) == 0 || strncmp(conv->model, "o1-", 3) == 0 ||
             strncmp(conv->model, "o3-", 3) == 0) {
            cfg.cloud_provider = CLOUD_PROVIDER_OPENAI;
         } else if (strncmp(conv->model, "claude-", 7) == 0) {
            cfg.cloud_provider = CLOUD_PROVIDER_CLAUDE;
         } else if (strncmp(conv->model, "gemini-", 7) == 0) {
            cfg.cloud_provider = CLOUD_PROVIDER_GEMINI;
         }
      }
   }

   /* The conversation's reasoning: its mode and its effort, both. */
   if (conv->thinking_mode[0] != '\0') {
      safe_strscpy(cfg.thinking_mode, conv->thinking_mode);
   }
   if (conv->reasoning_effort[0] != '\0') {
      safe_strscpy(cfg.reasoning_effort, conv->reasoning_effort);
   }
   *out = cfg;
   return true;
}

/* Apply a conversation's stored LLM settings to the session (no-op when it
 * stores none). */
static void webui_apply_conversation_llm_config(session_t *session, const conversation_t *conv) {
   session_llm_config_t base;
   session_llm_config_t cfg;
   session_get_llm_config(session, &base);
   if (conversation_llm_config(conv, &base, &cfg)) {
      session_set_llm_config(session, &cfg);
   }
}

/**
 * @brief Restore a conversation's LLM context into a session from DB
 *
 * Builds the history off-session, then installs it and binds @p conv_id in one
 * step (session_replace_history), so a concurrent turn never sees a half-built
 * or mislabelled history.  Applies the conversation's stored LLM settings.
 *
 * @return SUCCESS or FAILURE (DB read failed; history untouched)
 */
static int webui_restore_session_context(session_t *session,
                                         int user_id,
                                         const conversation_t *conv,
                                         int64_t conv_id,
                                         int *count_out) {
   if (count_out) {
      *count_out = 0;
   }
   bool has_system = false;
   int count = 0;
   json_object *hist = webui_build_conversation_context(user_id, conv, conv_id, true, &has_system,
                                                        &count);
   if (!hist) {
      return FAILURE;
   }
   if (!has_system) {
      /* SESSION_START builder boundary — clear dedup state so the next PER_TURN
       * admits all candidates fresh. */
      session_injected_set_clear(session);
   }
   session_replace_history(session, hist, conv_id);
   webui_apply_conversation_llm_config(session, conv);
   if (count_out) {
      *count_out = count;
   }
   return SUCCESS;
}

int webui_restore_conversation_context(ws_connection_t *conn,
                                       const conversation_t *conv,
                                       int64_t conv_id,
                                       int *count_out) {
   return webui_restore_session_context(conn->session, conn->auth_user_id, conv, conv_id,
                                        count_out);
}

/* session_turn_begin's loader: the context of a conversation other than the one
 * the session holds, built for that turn alone, plus its stored LLM settings.
 * Leaves the session (its history and LLM settings) untouched — those belong to
 * the conversation being viewed. */
json_object *webui_turn_history_loader(int user_id,
                                       int64_t conv_id,
                                       const session_llm_config_t *base,
                                       session_llm_config_t *cfg_out,
                                       bool *has_cfg_out) {
   *has_cfg_out = false;
   conversation_t conv;
   memset(&conv, 0, sizeof(conv));
   if (conv_db_get(conv_id, user_id, &conv) != AUTH_DB_SUCCESS) {
      conv_free(&conv);
      return NULL;
   }
   bool has_system = false;
   int count = 0;
   /* A turn rebuilds its system messages before every LLM call, so the loaded
    * copy needs only a placeholder, not the user's full prompt. */
   json_object *hist = webui_build_conversation_context(user_id, &conv, conv_id, false, &has_system,
                                                        &count);
   if (hist) {
      *has_cfg_out = conversation_llm_config(&conv, base, cfg_out);
   }
   conv_free(&conv);
   return hist;
}
