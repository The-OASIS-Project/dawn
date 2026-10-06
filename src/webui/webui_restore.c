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
#include "llm/llm_history_kind.h"
#include "llm/llm_interface.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "memory/memory_history_loader.h"
#include "utils/string_utils.h"
#include "webui/webui_handlers.h"
#include "webui/webui_internal.h"

/**
 * @brief Build a conversation's LLM context from DB, detached from any session
 *
 * The conversation's request as every loader rebuilds it
 * (memory_history_request_context): shared by sidebar load, session-expiry
 * recovery, and a turn whose conversation is not the one loaded
 * (session_turn_begin's loader).  Led by its frozen prefix when it stores one;
 * otherwise its next turn freezes the prompt it runs under.  Every restored
 * message carries its DB row id.  v67: bounded to the rows after the
 * compaction watermark, the summary standing in for the rest (the UI still
 * shows the whole transcript).
 *
 * @return New history array (caller owns), or NULL (DB read failed / OOM)
 */
static json_object *webui_build_conversation_context(int user_id,
                                                     const conversation_t *conv,
                                                     int64_t conv_id,
                                                     int *count_out) {
   return memory_history_request_context(conv_id, user_id, conv->context_watermark_msg_id,
                                         conv->compaction_summary, NULL, count_out);
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
   int count = 0;
   json_object *hist = webui_build_conversation_context(user_id, conv, conv_id, &count);
   if (!hist) {
      return FAILURE;
   }
   session_replace_history(session, hist, conv_id); /* a new context: its dedup state too */
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
   int count = 0;
   json_object *hist = webui_build_conversation_context(user_id, &conv, conv_id, &count);
   if (hist) {
      *has_cfg_out = conversation_llm_config(&conv, base, cfg_out);
   }
   conv_free(&conv);
   return hist;
}
