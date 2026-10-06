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
#include "auth/auth_db_conv_prefix.h"
#include "auth/auth_db_messages.h"
#include "auth/auth_db_withdraw.h"
#include "config/dawn_config.h"
#include "core/focus/focus_handles.h"
#include "core/prefix_in_force.h"
#include "core/session_compaction.h"
#include "core/session_history.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "core/tool_result_store.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_history_rows.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "memory/memory_extraction.h"
#include "utils/string_utils.h"

static const char *row_field(struct json_object *row, const char *key) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(row, key, &v) ? json_object_get_string(v) : NULL;
}

/* A voice save's rows, written in one transaction: what a compaction took out
 * first, then the history. */
typedef struct {
   conv_message_row_t *rows;
   int n;
   int cap;
   struct json_object *keep; /* the row objects the rows' strings belong to */
   struct {
      struct json_object *msg; /* stamped with its row id; NULL = not */
      int row;                 /* its row: the question's, not its context's */
   } * stamps;
   int n_stamps;
   int cap_stamps;
} row_batch_t;

static bool batch_grow(row_batch_t *b) {
   if (b->n < b->cap) {
      return true;
   }
   const int cap = b->cap ? b->cap * 2 : 64;
   conv_message_row_t *rows = realloc(b->rows, (size_t)cap * sizeof(*rows));
   if (!rows) {
      return false;
   }
   b->rows = rows;
   b->cap = cap;
   return true;
}

static bool batch_stamp(row_batch_t *b, struct json_object *obj, int row) {
   if (b->n_stamps == b->cap_stamps) {
      const int cap = b->cap_stamps ? b->cap_stamps * 2 : 32;
      void *grown = realloc(b->stamps, (size_t)cap * sizeof(*b->stamps));
      if (!grown) {
         return false;
      }
      b->stamps = grown;
      b->cap_stamps = cap;
   }
   b->stamps[b->n_stamps].msg = obj;
   b->stamps[b->n_stamps].row = row;
   b->n_stamps++;
   return true;
}

/* Add @p rows (llm_history_rows_append's shape; taken) as one message's rows:
 * @p msg is stamped with its own row's id (may be NULL), or, with @p each_row,
 * every row object with its own (the rows are the messages, as a compaction
 * kept them: memory extraction reads them, and trusts only saved rows). */
static bool batch_add(row_batch_t *b,
                      struct json_object *rows,
                      struct json_object *msg,
                      bool each_row) {
   json_object_array_add(b->keep, rows);
   const int n = (int)json_object_array_length(rows);
   int question = 0; /* 1-based, in the batch */
   const int first = b->n;
   for (int r = 0; r < n; r++) {
      struct json_object *row = json_object_array_get_idx(rows, r);
      if (!batch_grow(b)) {
         return false;
      }
      struct json_object *calls = NULL;
      json_object_object_get_ex(row, "tool_calls", &calls);
      struct json_object *images = NULL;
      json_object_object_get_ex(row, LLM_HISTORY_ROW_IMAGES_KEY, &images);
      const message_kind_t kind = llm_history_kind_of(row);
      /* A question's context rows follow its own row and name it. */
      b->rows[b->n] = (conv_message_row_t){
         .role = row_field(row, "role"),
         .content = row_field(row, "content"),
         .tool_calls = calls ? json_object_to_json_string_ext(calls, JSON_C_TO_STRING_PLAIN) : NULL,
         .tool_call_id = row_field(row, "tool_call_id"),
         .llm_blocks = row_field(row, LLM_HISTORY_ROW_STORED_KEY),
         .kind = row_field(row, MESSAGE_KIND_KEY),
         .context_of_row = kind != MESSAGE_KIND_NONE ? question : 0,
         /* Bound as the save's last step (conv_db_bind_images), so a save
          * that fails leaves them for its retry. */
         .images = images ? json_object_to_json_string_ext(images, JSON_C_TO_STRING_PLAIN) : NULL,
         .images_bind_later = true,
      };
      b->n++;
      if (each_row && !batch_stamp(b, row, b->n - 1)) {
         return false;
      }
      if (question == 0 && (kind == MESSAGE_KIND_NONE || kind == MESSAGE_KIND_ENVELOPE)) {
         question = b->n;
      }
   }
   if (!msg || b->n == first) {
      return true;
   }
   return batch_stamp(b, msg, question > 0 ? question - 1 : first);
}

static void batch_free(row_batch_t *b) {
   free(b->rows);
   free(b->stamps);
   json_object_put(b->keep);
   memset(b, 0, sizeof(*b));
}

/* Whether @p msg is saved as rows: the prompt isn't (the frozen prefix is saved
 * with the conversation); a system message with a kind (a directive, an
 * instruction change) is. */
static bool is_saved_message(struct json_object *msg) {
   struct json_object *role_obj = NULL;
   return msg && json_object_object_get_ex(msg, "role", &role_obj) &&
          !(strcmp(json_object_get_string(role_obj), "system") == 0 &&
            llm_history_kind_of(msg) == MESSAGE_KIND_NONE);
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

   /* The prompt the next context starts with (each turn then personalizes
    * it).  Copied before taking the history lock. */
   char *next_prompt = get_command_prompt_dup();

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

   /* What the user forgot during its last turn goes before it is saved; what
    * they forget from here on is checked once the rows are stored. */
   const int64_t saving_at = (int64_t)time(NULL);
   int64_t saving_seq = 0;
   (void)conv_db_withdraw_seq(&saving_seq); /* 0 on failure: every one counts */
   session_prefix_voice_save_locked(session);

   int msg_count = (int)json_object_array_length(session->conversation_history);
   int exchange_count = 0; /* messages other than the system prompt and request context */
   for (int i = 0; i < msg_count; i++) {
      struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
      struct json_object *role = NULL;
      if (json_object_object_get_ex(msg, "role", &role) &&
          strcmp(json_object_get_string(role), "system") != 0 && !llm_history_is_context(msg)) {
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
      /* What a compaction took out goes with it, unsaved. */
      session_new_context_locked(session, next_prompt);
      session->last_interaction_complete = 0;
      pthread_mutex_unlock(&session->history_mutex);
      free(next_prompt);
      OLOG_INFO("Session %u: guest conversation not saved (no user)", session->session_id);
      return 1;
   }

   /* The title: the first question's own words (not the context in front). */
   char title[128] = "Voice Conversation";
   for (int i = 0; i < msg_count; i++) {
      struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
      if (!llm_history_is_question(msg) || llm_history_kind_of(msg) != MESSAGE_KIND_NONE) {
         continue;
      }
      const char *content = llm_history_question_text(msg);
      if (content && content[0]) {
         /* Truncate to title length, add ellipsis if needed */
         const size_t max_len = sizeof(title) - 4; /* Room for "..." */
         if (strlen(content) <= max_len) {
            safe_strscpy(title, content);
         } else {
            memcpy(title, content, max_len);
            title[max_len] = '\0';
            utf8_trim_incomplete(title);
            strcat(title, "...");
         }
      }
      break;
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

   /* Each message as the rows it becomes (llm_history_rows_append): the text
    * and tool columns every reader expects, and an assistant turn's stored
    * blocks, so a reloaded voice conversation replays as it ran.  What a
    * compaction took out comes first (its rows, kept at the compaction): they
    * are summarized, the kept ones follow the watermark (session_compaction.h).
    * One transaction: a conversation saved whole or not at all. */
   session_compaction_t *cmp = &session->compaction;
   const int n_removed = cmp->voice_removed ? (int)json_object_array_length(cmp->voice_removed) : 0;
   row_batch_t batch = { .keep = json_object_new_array() };
   bool built = batch.keep != NULL;
   for (int i = 0; built && i < n_removed; i++) {
      built = batch_add(&batch, json_object_get(json_object_array_get_idx(cmp->voice_removed, i)),
                        NULL, true);
   }
   const int removed_rows = batch.n;
   for (int i = 0; built && i < msg_count; i++) {
      struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
      if (!is_saved_message(msg)) {
         continue;
      }
      struct json_object *rows = json_object_new_array();
      built = rows && llm_history_rows_append(msg, rows) >= 0 &&
              batch_add(&batch, rows, msg, false);
      if (!rows) {
         built = false;
      }
   }
   int64_t *ids = built && batch.n > 0 ? calloc((size_t)batch.n, sizeof(*ids)) : NULL;
   int saved = !built || (batch.n > 0 && !ids)
                   ? AUTH_DB_FAILURE
                   : conv_db_add_rows(conv_id, user_id, batch.rows, batch.n, ids);
   int64_t removed_first = removed_rows > 0 && ids ? ids[0] : 0;
   int64_t watermark = 0;
   for (int i = 0; saved == AUTH_DB_SUCCESS && i < removed_rows; i++) {
      if (ids[i] > watermark) {
         watermark = ids[i];
      }
   }
   /* The conversation's frozen prompt and tool set, as its turns were sent,
    * what is in force (the rows above include what each turn added), and the
    * compaction: without them a reload would replay what was summarized, so
    * they too are saved or the conversation isn't. */
   struct json_object *first = json_object_array_get_idx(session->conversation_history, 0);
   const bool compacted = cmp->voice_summary && watermark > 0;
   if (saved == AUTH_DB_SUCCESS &&
       (session_prefix_is_frozen(session->conversation_history) || compacted)) {
      const bool frozen = session_prefix_is_frozen(session->conversation_history);
      struct json_object *tools = NULL;
      if (frozen) {
         json_object_object_get_ex(first, LLM_HISTORY_TOOLS_KEY, &tools);
      }
      char *in_force = frozen ? prefix_in_force_json(session->conversation_history) : NULL;
      const conv_turn_save_t save = {
         .prefix = frozen ? row_field(first, "content") : NULL,
         .tools = tools ? json_object_to_json_string_ext(tools, JSON_C_TO_STRING_PLAIN) : NULL,
         .in_force = in_force,
         .compaction_summary = compacted ? cmp->voice_summary : NULL,
         .compaction_first_id = removed_first,
         .compaction_last_id = compacted ? watermark : 0,
         .compaction_level = cmp->voice_level,
      };
      const int kept = conv_db_save_turn(conv_id, user_id, &save, NULL);
      free(in_force);
      if (kept == AUTH_DB_INVALID) {
         /* It can never be stored (a prompt past the size a conversation keeps):
          * the conversation is kept without it, whole, rather than never saved.
          * Its rows then replay as they are, and the next seam compacts them. */
         OLOG_ERROR("Session %u: conv %lld's prompt or compaction can't be stored; saved "
                    "without them",
                    session->session_id, (long long)conv_id);
      } else {
         saved = kept;
      }
   }
   /* The results its turns stored, readable in it from now on, then the
    * images its tool rows name, the conversation's from now on.  Last: a
    * failure before them deletes the conversation, and would take them with
    * it.  The captures stay unbound until here, so they wait for the retry
    * (within IMAGE_UNBOUND_GRACE_SEC of their capture; the sweep spares any a
    * live session still holds past it: session_image_hold.c).  A capture
    * already reclaimed can never bind: the save goes on without it. */
   if (saved == AUTH_DB_SUCCESS) {
      saved = tool_result_store_bind_locked(session, conv_id, 0, true);
   }
   if (saved == AUTH_DB_SUCCESS) {
      saved = conv_db_bind_images(conv_id, user_id, NULL);
   }
   if (saved != AUTH_DB_SUCCESS) {
      OLOG_ERROR("Session %u: voice conversation %lld not saved (%d); tried again later",
                 session->session_id, (long long)conv_id, saved);
      /* The rollback: only database work (no image file is removed, so no
       * disk I/O under history_mutex).  Any capture this save bound goes
       * back to unbound for the retry; nothing a reply names, nor anything
       * another conversation names, is touched. */
      (void)conv_db_delete_ex(conv_id, user_id, false, CONV_IMAGES_UNBIND, NULL);
      batch_free(&batch);
      free(ids);
      pthread_mutex_unlock(&session->history_mutex);
      voice_save_later(session, false);
      free(next_prompt);
      return 1;
   }
   for (int i = 0; i < batch.n_stamps; i++) {
      json_object_object_add(batch.stamps[i].msg, "id",
                             json_object_new_int64(ids[batch.stamps[i].row]));
   }
   batch_free(&batch);
   free(ids);
   /* The handles its turn contexts number items with ([M3]), so a reopened
    * conversation keeps them and gives new items new ones. */
   (void)focus_handles_save_locked(session, conv_id, user_id);
   /* What the user forgot while it ran, now that its rows and handles are
    * stored (a turn built before the forgetting may have kept it). */
   (void)conv_db_withdraw_conversation(conv_id, user_id, saving_at, saving_seq);
   session_prefix_release_locked(session);

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
   /* What a compaction took out is part of what the conversation taught. */
   struct json_object *history_copy = NULL;
   if (g_config.memory.enabled) {
      struct json_object *whole = json_object_new_array();
      for (int i = 0; whole && i < n_removed; i++) {
         struct json_object *rows = json_object_array_get_idx(cmp->voice_removed, i);
         const size_t n = json_object_array_length(rows);
         for (size_t r = 0; r < n; r++) {
            json_object_array_add(whole, json_object_get(json_object_array_get_idx(rows, r)));
         }
      }
      for (int i = 0; whole && i < msg_count; i++) {
         json_object_array_add(
             whole, json_object_get(json_object_array_get_idx(session->conversation_history, i)));
      }
      history_copy = whole ? llm_history_strip_internal(whole) : NULL;
      json_object_put(whole);
   }
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
