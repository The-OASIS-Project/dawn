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
 * What a user forgot, for an extraction of a batch said before the forget:
 * loaded (memory_forgotten_load), then what the extraction returned is checked
 * against it (memory_forgotten_screen).  Written by memory_db_fact_forget.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "memory/memory_forgotten.h"

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "llm/llm_cache_monitor.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_extraction.h"
#include "prompts.h"
#include "utils/string_utils.h"

int memory_forgotten_load(int user_id,
                          int64_t conversation_id,
                          int64_t first_id,
                          int64_t since,
                          memory_forgotten_list_t *out) {
   memset(out, 0, sizeof(*out));
   if (user_id <= 0 || (first_id <= 0 && conversation_id <= 0 && since <= 0)) {
      return MEMORY_DB_SUCCESS; /* nothing saved: nothing said before a forget */
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = SQLITE_ERROR;
   int cap = 0;
   bool ok = true;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT id, fact_text FROM memory_forgotten WHERE user_id = ?1 AND "
                          "CASE WHEN ?3 > 0 THEN forget_msg_id >= ?3 WHEN ?2 > 0 THEN "
                          /* A history without row ids: its conversation's rows not
                           * extracted yet (none: nothing is loaded)... */
                          "forget_msg_id >= (SELECT MIN(m.id) FROM messages m JOIN conversations "
                          "c ON c.id = m.conversation_id WHERE c.id = ?2 AND c.user_id = ?1 AND "
                          "m.kind IS NULL AND m.id > c.last_extracted_msg_id) "
                          /* ...or, with no conversation (never saved), what was
                           * forgotten since it began. */
                          "ELSE created_at >= ?4 END ORDER BY id DESC",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, conversation_id);
      sqlite3_bind_int64(st, 3, first_id > 0 ? first_id : 0);
      sqlite3_bind_int64(st, 4, since > 0 ? since : INT64_MAX);
      while (ok && (rc = sqlite3_step(st)) == SQLITE_ROW) {
         if (out->count == cap) {
            const int grown = cap ? cap * 2 : 16;
            memory_forgotten_t *more = realloc(out->items, (size_t)grown * sizeof(*more));
            ok = more != NULL;
            if (!ok) {
               break;
            }
            out->items = more;
            cap = grown;
         }
         memory_forgotten_t *f = &out->items[out->count++];
         memset(f, 0, sizeof(*f));
         f->id = sqlite3_column_int64(st, 0);
         const char *text = (const char *)sqlite3_column_text(st, 1);
         safe_strscpy(f->text, text ? text : "");
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   out->user_id = user_id;
   if (!ok || rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: loading forgotten facts for user %d failed", user_id);
      free(out->items);
      memset(out, 0, sizeof(*out));
      return MEMORY_DB_FAILURE;
   }
   return MEMORY_DB_SUCCESS;
}

static void texts_free(memory_forgotten_texts_t *set) {
   for (int i = 0; i < set->count; i++) {
      free(set->from[i]);
      free(set->to[i]);
   }
   free(set->from);
   free(set->to);
   memset(set, 0, sizeof(*set));
}

void memory_forgotten_free(memory_forgotten_list_t *list) {
   if (!list) {
      return;
   }
   for (int i = 0; i < list->count; i++) {
      free(list->items[i].embedding);
   }
   texts_free(&list->drop);
   texts_free(&list->rewrite);
   free(list->items);
   list->items = NULL;
   list->count = 0;
}

/* @p text with ASCII case and runs of spacing folded, trailing spacing and
 * full stops trimmed: the same words as written, not a bag of words (so "not"
 * and word order still count).  Heap; NULL on allocation failure. */
static char *fold(const char *text) {
   const size_t len = strlen(text);
   char *out = malloc(len + 1);
   if (!out) {
      return NULL;
   }
   size_t n = 0;
   bool space = false;
   for (const char *c = text; *c; c++) {
      if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') {
         space = n > 0;
         continue;
      }
      if (space) {
         out[n++] = ' ';
         space = false;
      }
      out[n++] = (*c >= 'A' && *c <= 'Z') ? (char)(*c - 'A' + 'a') : *c;
   }
   while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ' ')) {
      n--;
   }
   out[n] = '\0';
   return out;
}

static bool same_text(const char *a, const char *b) {
   char *fa = fold(a);
   char *fb = fold(b);
   const bool same = fa && fb && strcmp(fa, fb) == 0;
   free(fa);
   free(fb);
   return same;
}

/* Where @p text (folded) is in @p set, or -1. */
static int texts_find(const memory_forgotten_texts_t *set, const char *text) {
   char *folded = text ? fold(text) : NULL;
   int at = -1;
   for (int i = 0; folded && i < set->count && at < 0; i++) {
      if (strcmp(set->from[i], folded) == 0) {
         at = i;
      }
   }
   free(folded);
   return at;
}

/* @p text (folded) into @p set, with @p to (copied; may be NULL), once. */
static int texts_add(memory_forgotten_texts_t *set, const char *text, const char *to) {
   if (texts_find(set, text) >= 0) {
      return SUCCESS;
   }
   if (set->count == set->cap) {
      const int grown = set->cap ? set->cap * 2 : 16;
      char **from = realloc(set->from, (size_t)grown * sizeof(*from));
      if (from) {
         set->from = from;
      }
      char **too = from ? realloc(set->to, (size_t)grown * sizeof(*too)) : NULL;
      if (!too) {
         return FAILURE;
      }
      set->to = too;
      set->cap = grown;
   }
   char *f = fold(text);
   char *t = to ? strdup(to) : NULL;
   if (!f || (to && !t)) {
      free(f);
      free(t);
      return FAILURE;
   }
   set->from[set->count] = f;
   set->to[set->count++] = t;
   return SUCCESS;
}

/* Whether @p text is, word for word, a forgotten fact. */
static bool names_forgotten(const memory_forgotten_list_t *list, const char *text) {
   for (int i = 0; text && i < list->count; i++) {
      if (!list->items[i].known && same_text(text, list->items[i].text)) {
         return true;
      }
   }
   return false;
}

/* Store @p text as @p to (the model's rewording without a forgotten fact).
 * Nothing left of it, a rewording too long to store, or one that is a forgotten
 * fact itself: not stored at all. */
static int rewrite_add(memory_forgotten_list_t *list, const char *text, const char *to) {
   char *folded_to = fold(to);
   const bool empty = folded_to && !folded_to[0];
   free(folded_to);
   if (empty || strlen(to) >= MEMORY_FACT_TEXT_MAX || names_forgotten(list, to)) {
      return texts_add(&list->drop, text, NULL);
   }
   return same_text(text, to) ? SUCCESS : texts_add(&list->rewrite, text, to);
}

/* @p f's embedding, made on first use (the provider may be remote). */
static void embed_item(memory_forgotten_t *f) {
   if (f->embedded) {
      return;
   }
   f->embedded = true;
   float vec[MAX_EMBEDDING_DIMS];
   int dims = 0;
   if (memory_embeddings_embed(f->text, vec, &dims) != SUCCESS || dims <= 0) {
      return;
   }
   f->embedding = malloc((size_t)dims * sizeof(float));
   if (f->embedding) {
      memcpy(f->embedding, vec, (size_t)dims * sizeof(float));
      f->dims = dims;
      f->norm = memory_embeddings_l2_norm(vec, dims);
   }
}

/* The returned facts' texts: facts[].text and corrections[].new_fact, into
 * @p out (room for every element of both arrays).  How many. */
static int returned_texts(struct json_object *root, const char **out) {
   static const char *const k_arrays[][2] = { { "facts", "text" }, { "corrections", "new_fact" } };
   int n = 0;
   for (size_t a = 0; a < sizeof(k_arrays) / sizeof(k_arrays[0]); a++) {
      struct json_object *arr = NULL;
      if (!json_object_object_get_ex(root, k_arrays[a][0], &arr) ||
          !json_object_is_type(arr, json_type_array)) {
         continue;
      }
      for (size_t i = 0; i < json_object_array_length(arr); i++) {
         struct json_object *t = NULL;
         if (!json_object_object_get_ex(json_object_array_get_idx(arr, i), k_arrays[a][1], &t)) {
            continue;
         }
         const char *text = json_object_get_string(t);
         if (text && text[0]) {
            out[n++] = text;
         }
      }
   }
   return n;
}

/* Room returned_texts needs for @p root. */
static size_t returned_room(struct json_object *root) {
   size_t room = 0;
   struct json_object *arr = NULL;
   if (json_object_object_get_ex(root, "facts", &arr) &&
       json_object_is_type(arr, json_type_array)) {
      room += json_object_array_length(arr);
   }
   if (json_object_object_get_ex(root, "corrections", &arr) &&
       json_object_is_type(arr, json_type_array)) {
      room += json_object_array_length(arr);
   }
   return room;
}

/* Whether @p text is close to a forgotten fact (any, without embeddings);
 * marks the ones it is close to as asked about. */
static bool is_candidate(memory_forgotten_list_t *list, const char *text) {
   float vec[MAX_EMBEDDING_DIMS];
   int dims = 0;
   const bool embedded = memory_embeddings_available() &&
                         memory_embeddings_embed(text, vec, &dims) == SUCCESS && dims > 0;
   const float norm = embedded ? memory_embeddings_l2_norm(vec, dims) : 0.0f;
   bool close = false;
   for (int i = 0; i < list->count; i++) {
      memory_forgotten_t *f = &list->items[i];
      if (f->known) {
         continue;
      }
      if (embedded && norm > 0.0f) {
         embed_item(f);
      }
      /* Without a vector on either side there is nothing to rule it out. */
      if (!embedded || norm <= 0.0f || !f->embedding || f->dims != dims || f->norm <= 0.0f ||
          memory_embeddings_cosine_with_norms(vec, f->embedding, dims, norm, f->norm) >=
              MEMORY_FORGOTTEN_CANDIDATE) {
         f->asked = true;
         close = true;
      }
   }
   return close;
}

/* The question for @p texts against the asked-about forgotten facts.  Heap;
 * NULL on allocation failure. */
static char *build_question(const memory_forgotten_list_t *list, const char *const *texts, int n) {
   strbuf_t old;
   strbuf_t new_facts;
   strbuf_init(&old, 1024);
   strbuf_init(&new_facts, 2048);
   for (int i = 0; i < list->count; i++) {
      if (list->items[i].asked) {
         strbuf_appendf(&old, "- %s\n", list->items[i].text);
      }
   }
   for (int i = 0; i < n; i++) {
      strbuf_appendf(&new_facts, "%d. %s\n", i + 1, texts[i]);
   }
   char *old_s = strbuf_steal_or_null(&old);
   char *new_s = strbuf_steal_or_null(&new_facts);
   const int len = old_s && new_s ? snprintf(NULL, 0, MEMORY_FORGOTTEN_CHECK_PROMPT, old_s, new_s)
                                  : -1;
   char *prompt = len > 0 ? malloc((size_t)len + 1) : NULL;
   if (prompt) {
      snprintf(prompt, (size_t)len + 1, MEMORY_FORGOTTEN_CHECK_PROMPT, old_s, new_s);
   }
   free(old_s);
   free(new_s);
   return prompt;
}

/* @p entry as a fact's number: an integer, a whole double or a numeric string
 * ("2"); false for anything else. */
static bool answer_number(struct json_object *entry, int *k) {
   if (json_object_is_type(entry, json_type_int)) {
      *k = json_object_get_int(entry);
      return true;
   }
   if (json_object_is_type(entry, json_type_double)) {
      const double d = json_object_get_double(entry);
      if (!(d >= -1.0e9 && d <= 1.0e9)) {
         return false; /* NaN, infinite, or no fact's number */
      }
      *k = (int)d;
      return d == (double)*k;
   }
   if (json_object_is_type(entry, json_type_string)) {
      const char *str = json_object_get_string(entry);
      char *end = (char *)str;
      const long v = strtol(str, &end, 10);
      while (*end == ' ') {
         end++;
      }
      *k = (int)v;
      return end != str && *end == '\0' && v == (long)*k;
   }
   return false;
}

/* Take the model's answer to the question about @p texts: those it names
 * "same" into the drop set, rewordings into the rewrite set.  FAILURE: not the
 * JSON asked for (or no memory). */
static int take_answer(memory_forgotten_list_t *list,
                       const char *response,
                       const char *const *texts,
                       int n) {
   struct json_object *root = response ? memory_extraction_parse_json(response) : NULL;
   struct json_object *same = NULL;
   if (!root || !json_object_object_get_ex(root, "same", &same) ||
       !json_object_is_type(same, json_type_array)) {
      json_object_put(root);
      return FAILURE;
   }
   int rc = SUCCESS;
   for (size_t i = 0; rc == SUCCESS && i < json_object_array_length(same); i++) {
      int k = 0;
      if (!answer_number(json_object_array_get_idx(same, i), &k)) {
         rc = FAILURE; /* not the answer asked for: asked again, or not stored */
         break;
      }
      if (k >= 1 && k <= n) {
         rc = texts_add(&list->drop, texts[k - 1], NULL);
      }
   }
   /* What stays, without a forgotten fact it still mentions. */
   struct json_object *rewrite = NULL;
   if (rc == SUCCESS && json_object_object_get_ex(root, "rewrite", &rewrite) &&
       json_object_is_type(rewrite, json_type_object)) {
      json_object_object_foreach(rewrite, key, val) {
         const int k = atoi(key);
         if (rc == SUCCESS && k >= 1 && k <= n && json_object_is_type(val, json_type_string) &&
             texts_find(&list->drop, texts[k - 1]) < 0) {
            rc = rewrite_add(list, texts[k - 1], json_object_get_string(val));
         }
      }
   }
   json_object_put(root);
   return rc;
}

/* Ask the model which of @p texts state an asked-about forgotten fact; tried
 * twice (a reply that isn't the JSON asked for). */
static int ask_same(memory_forgotten_list_t *list,
                    const char *const *texts,
                    int n,
                    const llm_resolved_config_t *cfg) {
   char *prompt = build_question(list, texts, n);
   int rc = FAILURE;
   for (int attempt = 0; prompt && attempt < 2 && rc != SUCCESS; attempt++) {
      struct json_object *history = json_object_new_array();
      struct json_object *msg = json_object_new_object();
      json_object_object_add(msg, "role", json_object_new_string("user"));
      json_object_object_add(msg, "content", json_object_new_string(prompt));
      json_object_array_add(history, msg);
      /* Text from a conversation: no tools, as for the extraction itself. */
      llm_tools_suppress_push();
      const int kind_prev = llm_cache_monitor_push_kind(LLM_CALL_EXTRACTION);
      char *response = llm_chat_completion_with_config(history, NULL, cfg);
      llm_cache_monitor_pop_kind(kind_prev);
      llm_tools_suppress_pop();
      json_object_put(history);
      /* A half-taken answer is taken again whole on the retry. */
      texts_free(&list->drop);
      texts_free(&list->rewrite);
      rc = take_answer(list, response, texts, n);
      free(response);
   }
   free(prompt);
   return rc;
}

int memory_forgotten_screen(memory_forgotten_list_t *list,
                            const char *response,
                            const llm_resolved_config_t *cfg) {
   if (!list || list->count == 0 || !response) {
      return SUCCESS;
   }
   struct json_object *root = memory_extraction_parse_json(response);
   if (!root) {
      return SUCCESS; /* nothing will be stored from it either */
   }
   texts_free(&list->drop);
   texts_free(&list->rewrite);
   /* One copy of a fact deleted while another is kept: still known, so what
    * says it isn't dropped. */
   for (int i = 0; i < list->count; i++) {
      memory_forgotten_t *f = &list->items[i];
      f->asked = false;
      if (!f->embedded && memory_embeddings_available()) {
         embed_item(f);
         int64_t live = 0;
         float score = 0.0f;
         if (f->embedding) {
            memory_embeddings_nearest_fact(list->user_id, f->embedding, f->dims,
                                           MEMORY_FORGOTTEN_STILL_KNOWN, &live, &score);
         }
         f->known = live > 0;
      }
   }
   const size_t room = returned_room(root);
   const char **texts = room ? malloc(room * sizeof(*texts)) : NULL;
   const char **ask = room ? malloc(room * sizeof(*ask)) : NULL;
   if (room && (!texts || !ask)) {
      free(texts);
      free(ask);
      json_object_put(root);
      return FAILURE;
   }
   const int n_texts = room ? returned_texts(root, texts) : 0;
   int n_ask = 0;
   for (int t = 0; t < n_texts; t++) {
      if (!names_forgotten(list, texts[t]) && is_candidate(list, texts[t])) {
         ask[n_ask++] = texts[t];
      }
   }
   /* The same words as a forgotten fact: dropped without asking. */
   int rc = n_ask > 0 && ask_same(list, ask, n_ask, cfg) != SUCCESS ? FAILURE : SUCCESS;
   if (rc != SUCCESS) {
      /* What it would have asked about isn't stored; the rest is. */
      OLOG_WARNING("memory_extraction: checking %d fact(s) against forgotten ones failed; not "
                   "storing them",
                   n_ask);
      texts_free(&list->drop);
      texts_free(&list->rewrite);
      rc = SUCCESS;
      for (int t = 0; rc == SUCCESS && t < n_ask; t++) {
         rc = texts_add(&list->drop, ask[t], NULL);
      }
   }
   for (int t = 0; rc == SUCCESS && t < n_texts; t++) {
      if (names_forgotten(list, texts[t])) {
         rc = texts_add(&list->drop, texts[t], NULL);
      }
   }
   free(texts);
   free(ask);
   json_object_put(root);
   return rc;
}

bool memory_forgotten_is_repeat(const memory_forgotten_list_t *list,
                                const char *text,
                                int64_t conversation_id) {
   if (!list || list->drop.count == 0 || !text) {
      return false;
   }
   const bool found = texts_find(&list->drop, text) >= 0;
   if (found) {
      OLOG_INFO("memory_extraction: conv %lld: a fact stating a forgotten one again not stored",
                (long long)conversation_id);
   }
   return found;
}

const char *memory_forgotten_rewritten(const memory_forgotten_list_t *list, const char *text) {
   const int at = list ? texts_find(&list->rewrite, text) : -1;
   if (at < 0) {
      return NULL;
   }
   OLOG_INFO("memory_extraction: a fact stored without a forgotten one it mentioned");
   return list->rewrite.to[at];
}

bool memory_forgotten_names(const memory_forgotten_list_t *list, const char *text) {
   return list && names_forgotten(list, text);
}

void memory_forgotten_saved(int user_id, int64_t since, int64_t conv_id) {
   if (user_id <= 0 || conv_id <= 0) {
      return;
   }
   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE memory_forgotten SET forget_msg_id = MAX(forget_msg_id, "
                          "(SELECT COALESCE(MAX(m.id), 0) FROM messages m WHERE "
                          "m.conversation_id = ?3 AND m.kind IS NULL)) WHERE user_id = ?1 AND "
                          "created_at >= ?2",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, since);
      sqlite3_bind_int64(st, 3, conv_id);
      if (sqlite3_step(st) != SQLITE_DONE) {
         OLOG_WARNING("memory: moving forgets past conversation %lld failed: %s",
                      (long long)conv_id, sqlite3_errmsg(s_db.db));
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
}

int memory_forgotten_sweep(int64_t older_than) {
   AUTH_DB_LOCK_OR_RETURN(0);
   /* Read by no extraction any more: none of the user's messages is from
    * before the forget (those conversations deleted).  A young one may still
    * be moved past a voice session's save (memory_forgotten_saved). */
   sqlite3_stmt *st = NULL;
   int removed = 0;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "DELETE FROM memory_forgotten WHERE created_at < ?1 AND "
                               "forget_msg_id < COALESCE((SELECT MIN(m.id) FROM conversations c "
                               "JOIN messages m ON m.conversation_id = c.id WHERE c.user_id = "
                               "memory_forgotten.user_id AND m.kind IS NULL), forget_msg_id + 1)",
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, older_than);
      rc = sqlite3_step(st);
      removed = rc == SQLITE_DONE ? sqlite3_changes(s_db.db) : 0;
   }
   if (rc != SQLITE_DONE) {
      OLOG_WARNING("memory: sweeping forgotten-fact records failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (removed > 0) {
      OLOG_INFO("memory: %d forgotten fact record(s) no conversation reads removed", removed);
   }
   return removed;
}
