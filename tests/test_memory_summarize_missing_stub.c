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
 * Stubs for test_memory_summarize_missing.
 *
 * The unit test only exercises memory_summarize_missing_count + the
 * start/stop input-validation paths, not the worker body.  Every symbol
 * here is link-time only.  An abort() guard in each LLM / storage stub
 * makes it loud if a future test accidentally drags the worker path in
 * without supplying real implementations.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "llm/llm_cache_monitor.h"
#include "memory/memory_db.h"
#include "memory/memory_extraction.h"
#include "memory/memory_history_loader.h"
#include "memory/memory_types.h"

/* Globals required by anything that pulls memory headers. */
auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
};
dawn_config_t g_config;

/* memory_extraction_resolve_config — abort guard so unintended worker
 * invocations are immediately obvious. */
int memory_extraction_resolve_config(llm_resolved_config_t *cfg,
                                     char *model_buf,
                                     size_t model_buf_sz,
                                     char *endpoint_buf,
                                     size_t endpoint_buf_sz,
                                     const char *log_prefix) {
   (void)cfg;
   (void)model_buf;
   (void)model_buf_sz;
   (void)endpoint_buf;
   (void)endpoint_buf_sz;
   (void)log_prefix;
   fprintf(stderr, "memory_extraction_resolve_config stub: worker path is not under test\n");
   abort();
}

/* memory_extraction_parse_json — same abort guard. */
struct json_object *memory_extraction_parse_json(const char *response) {
   (void)response;
   fprintf(stderr, "memory_extraction_parse_json stub invoked\n");
   abort();
}

/* History loader — no-op stubs (worker is not under test). */
struct json_object *memory_history_load_from_db(int64_t conv_id,
                                                int user_id,
                                                size_t *text_len_out) {
   (void)conv_id;
   (void)user_id;
   if (text_len_out)
      *text_len_out = 0;
   return NULL;
}

struct json_object *memory_extraction_build_input(int user_id,
                                                  int64_t conversation_id,
                                                  struct json_object *conversation_history,
                                                  int64_t last_msg_id) {
   (void)user_id;
   (void)conversation_id;
   (void)conversation_history;
   (void)last_msg_id;
   return NULL;
}

struct json_object *llm_history_strip_internal(struct json_object *history) {
   (void)history;
   return NULL;
}

/* Conversation DB stubs.  conv_db_get_anchor_date returns NOT_FOUND-ish so
 * the worker would build no anchor line; conv_db_get_max_msg_id likewise
 * returns 0.  Neither is reached in the count-only tests. */
int conv_db_get_anchor_date(int64_t conv_id, int64_t *anchor_out) {
   (void)conv_id;
   if (anchor_out)
      *anchor_out = 0;
   return AUTH_DB_FAILURE;
}

int conv_db_get_created_at(int64_t conv_id, int64_t *created_at_out) {
   (void)conv_id;
   if (created_at_out)
      *created_at_out = 0; /* worker treats 0 as "no override", uses NOW() */
   return AUTH_DB_NOT_FOUND;
}

int conv_db_get_max_msg_id(int64_t conv_id, int user_id, int64_t *max_id_out) {
   (void)conv_id;
   (void)user_id;
   if (max_id_out)
      *max_id_out = 0;
   return AUTH_DB_FAILURE;
}

/* memory_db_summary_create_at — abort, worker storage path. */
int memory_db_summary_create_at(int user_id,
                                const char *session_id,
                                const char *summary,
                                const char *topics,
                                const char *sentiment,
                                int message_count,
                                int duration_seconds,
                                const memory_provenance_t *prov,
                                int64_t created_at_override,
                                int64_t *id_out) {
   (void)user_id;
   (void)session_id;
   (void)summary;
   (void)topics;
   (void)sentiment;
   (void)message_count;
   (void)duration_seconds;
   (void)prov;
   (void)created_at_override;
   (void)id_out;
   fprintf(stderr, "memory_db_summary_create_at stub invoked\n");
   abort();
}

/* memory_embeddings_embed_and_store_summary — abort.  Live extraction +
 * summarize-missing both call this post-create; the count-only test
 * paths never exercise the store path so this should never fire.  If it
 * does, the test is exercising an unexpected branch. */
int memory_embeddings_embed_and_store_summary(int user_id,
                                              int64_t summary_id,
                                              const char *summary_text) {
   (void)user_id;
   (void)summary_id;
   (void)summary_text;
   fprintf(stderr, "memory_embeddings_embed_and_store_summary stub invoked\n");
   abort();
}

/* memory_filter_check — never triggered in count-only tests; provide a
 * pass-through. */
int memory_filter_check(const char *text) {
   (void)text;
   return 0;
}

/* Call-kind tags for the cache telemetry: not under test here. */
int llm_cache_monitor_push_kind(llm_call_kind_t kind) {
   (void)kind;
   return -1;
}
void llm_cache_monitor_pop_kind(int previous) {
   (void)previous;
}

/* llm_chat_completion_with_config — abort guard.  Signature must match the
 * header exactly or the linker / compiler rejects the redeclaration. */
char *llm_chat_completion_with_config(struct json_object *conversation_history,
                                      const char *prompt,
                                      const llm_resolved_config_t *cfg) {
   (void)conversation_history;
   (void)prompt;
   (void)cfg;
   fprintf(stderr, "llm_chat_completion_with_config stub invoked\n");
   abort();
}
