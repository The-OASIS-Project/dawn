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
 * Saving a voice session's conversation: the local mic's and a satellite's,
 * whose turns live only in the session until it goes idle.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "config/dawn_config.h"
#include "core/session_history.h"
#include "core/session_manager.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_history_rows.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "memory/memory_extraction.h"
#include "utils/string_utils.h"

static const char *row_field(struct json_object *row, const char *key) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(row, key, &v) ? json_object_get_string(v) : NULL;
}

/* Write one row (llm_history_rows_append's shape); its id, or 0. */
static int64_t save_row(int64_t conv_id, int user_id, struct json_object *row) {
   struct json_object *calls = NULL;
   json_object_object_get_ex(row, "tool_calls", &calls);
   const conv_message_row_t db_row = {
      .role = row_field(row, "role"),
      .content = row_field(row, "content"),
      .tool_calls = calls ? json_object_to_json_string_ext(calls, JSON_C_TO_STRING_PLAIN) : NULL,
      .tool_call_id = row_field(row, "tool_call_id"),
      .llm_blocks = row_field(row, LLM_HISTORY_ROW_STORED_KEY),
   };
   int64_t id = 0;
   if (conv_db_add_row(conv_id, user_id, &db_row, &id) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("voice save: a %s row not saved to conv %lld", db_row.role ? db_row.role : "?",
                   (long long)conv_id);
      return 0;
   }
   return id;
}

/* Seconds before an idle voice save that couldn't run (a turn in progress, a
 * database error) is tried again, rather than on every idle check. */
#define VOICE_SAVE_RETRY_SEC 60

/* A voice save that couldn't run is tried again later; one with nothing to
 * save waits for the next interaction. */
static void voice_save_later(session_t *session, bool nothing_to_save) {
   const time_t timeout = (time_t)g_config.memory.conversation_idle_timeout_min * 60;
   if (nothing_to_save || timeout <= 0) {
      session->last_interaction_complete = 0;
   } else if (session->last_interaction_complete > 0) {
      session->last_interaction_complete = time(NULL) - timeout + VOICE_SAVE_RETRY_SEC;
   }
}

int session_save_voice_conversation(session_t *session, int64_t *conv_id_out) {
   if (conv_id_out)
      *conv_id_out = 0;

   if (!session || !conv_id_out) {
      return 1;
   }

   /* The prompt the next context starts with: the local mic's, or the remote
    * one a satellite starts with (each turn then personalizes it).  Copied
    * before taking the history lock. */
   char *next_prompt = session->type == SESSION_TYPE_LOCAL ? get_local_command_prompt_dup()
                                                           : get_remote_command_prompt_dup();

   pthread_mutex_lock(&session->history_mutex);

   /* Not while a turn is running: it reads this history without the lock, and
    * its exchange isn't finished (the next idle check saves it). */
   if (session->turn_active) {
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_INFO("Session %u: turn in progress; voice conversation not saved yet",
                session->session_id);
      voice_save_later(session, false);
      free(next_prompt);
      return 1;
   }

   /* Check if there are messages to save */
   if (!session->conversation_history) {
      pthread_mutex_unlock(&session->history_mutex);
      free(next_prompt);
      return 1;
   }

   int msg_count = (int)json_object_array_length(session->conversation_history);
   int exchange_count = 0; /* messages other than the system prompt */
   for (int i = 0; i < msg_count; i++) {
      struct json_object *role = NULL;
      if (json_object_object_get_ex(json_object_array_get_idx(session->conversation_history, i),
                                    "role", &role) &&
          strcmp(json_object_get_string(role), "system") != 0) {
         exchange_count++;
      }
   }
   if (exchange_count == 0) {
      /* Just the system prompt */
      pthread_mutex_unlock(&session->history_mutex);
      voice_save_later(session, true);
      free(next_prompt);
      return 1;
   }

   /* A guest's conversation (an unmapped satellite) belongs to no one: it is
    * not saved, and the next context starts fresh. */
   const int user_id = session_effective_user_id(session);
   if (user_id <= 0) {
      session_fact_source_t dropped[SESSION_PENDING_FACT_SOURCES_MAX];
      (void)session_take_fact_sources_locked(session, dropped);
      session_new_context_locked(session, next_prompt);
      session->last_interaction_complete = 0;
      pthread_mutex_unlock(&session->history_mutex);
      free(next_prompt);
      OLOG_INFO("Session %u: guest conversation not saved (no user)", session->session_id);
      return 1;
   }

   /* Generate title from first user message */
   char title[128] = "Voice Conversation";
   for (int i = 0; i < msg_count; i++) {
      struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
      if (!msg)
         continue;

      struct json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;

      const char *role = json_object_get_string(role_obj);
      if (role && strcmp(role, "user") == 0) {
         struct json_object *content_obj;
         if (json_object_object_get_ex(msg, "content", &content_obj)) {
            const char *content = json_object_get_string(content_obj);
            if (content && strlen(content) > 0) {
               /* Truncate to title length, add ellipsis if needed */
               size_t max_len = sizeof(title) - 4; /* Room for "..." */
               if (strlen(content) <= max_len) {
                  safe_strscpy(title, content);
               } else {
                  strncpy(
                      title, content,
                      max_len); /* strncpy-ok: computed truncation length (title[128], max_len=124),
                                   NUL-terminated below then room reserved for "..." */
                  title[max_len] = '\0';
                  strcat(title, "...");
               }
               title[sizeof(title) - 1] = '\0';
            }
         }
         break;
      }
   }

   /* Create conversation in database with voice origin */
   int64_t conv_id = 0;
   int rc = conv_db_create_with_origin(user_id, title, "voice", &conv_id);
   if (rc != AUTH_DB_SUCCESS) {
      OLOG_ERROR("Session %u: Failed to create voice conversation: %d", session->session_id, rc);
      pthread_mutex_unlock(&session->history_mutex);
      voice_save_later(session, false);
      free(next_prompt);
      return 1;
   }

   /* Save each message as the rows it becomes (llm_history_rows_append): the
    * text and tool columns every reader expects, and an assistant turn's
    * stored blocks, so a reloaded voice conversation replays as it ran. */
   struct json_object *rows = json_object_new_array();
   for (int i = 0; rows && i < msg_count; i++) {
      struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
      struct json_object *role_obj = NULL;
      /* The prompt isn't part of the conversation (and its volatile block
       * carries this turn's context and device events). */
      if (!msg || !json_object_object_get_ex(msg, "role", &role_obj) ||
          strcmp(json_object_get_string(role_obj), "system") == 0) {
         continue;
      }
      const int from = (int)json_object_array_length(rows);
      const int added = llm_history_rows_append(msg, rows);
      for (int r = from; r < from + added; r++) {
         const int64_t row_id = save_row(conv_id, user_id, json_object_array_get_idx(rows, r));
         if (r == from && row_id > 0) {
            json_object_object_add(msg, "id", json_object_new_int64(row_id));
         }
      }
   }
   json_object_put(rows);

   /* Trigger memory extraction (async) if enabled.
    *
    * NOTE: a background job never reaches here.  Job sessions are allocated bare
    * (session_manager_alloc_bare) and torn down by job_manager_end() via
    * session_manager_free_bare() -> session_free(), so neither this function nor
    * session_destroy() runs for them.  A SESSION_TYPE_JOB guard on this line
    * would be dead code: memory_trigger_extraction() itself refuses background-job
    * and private conversations, for every caller. */
   /* Deep-copy history with DAWN's own keys stripped: a turn's blocks carry
    * reasoning a vendor issued for itself, which must not reach the
    * memory-extraction LLM (which may be Claude/Gemini/local).  Extraction
    * starts after the lock is released: building its fallback reads the LLM
    * settings under llm_config_mutex, never held together with this one. */
   struct json_object *history_copy = g_config.memory.enabled ? llm_history_strip_internal(
                                                                    session->conversation_history)
                                                              : NULL;

   /* Start the next context in the same critical section, so no turn can begin
    * on the history just saved (its exchange would be saved twice, or lost),
    * taking the facts the voice turns saved to memory: they were learned in
    * this conversation. */
   session_fact_source_t facts[SESSION_PENDING_FACT_SOURCES_MAX];
   const int fact_count = session_take_fact_sources_locked(session, facts);
   session_new_context_locked(session, next_prompt);
   session->last_interaction_complete = 0;

   pthread_mutex_unlock(&session->history_mutex);
   free(next_prompt);

   session_record_fact_sources(facts, fact_count, conv_id, user_id);

   if (history_copy) {
      int duration_seconds = (int)(time(NULL) - session->created_at);
      char session_id_str[32];
      snprintf(session_id_str, sizeof(session_id_str), "voice_%u", session->session_id);
      memory_extraction_fallback_t fb;
      memory_extraction_build_fallback(session, &fb);
      memory_trigger_extraction(user_id, conv_id, session_id_str, history_copy, msg_count,
                                duration_seconds, &fb);
   }

   OLOG_INFO("Session %u: Saved voice conversation %lld (%d messages, user %d)",
             session->session_id, (long long)conv_id, exchange_count, user_id);

   *conv_id_out = conv_id;
   return 0;
}
