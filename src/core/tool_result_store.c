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
 * Tool results kept whole behind a view (tool_result_store.h).
 */

#define _GNU_SOURCE /* memrchr */
#include "core/tool_result_store.h"

#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#include "auth/auth_db.h"
#include "blob_store.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "llm/llm_tool_view.h"
#include "logging.h"

/* ---------------------------------------------------------------------------
 * The minted set: the unbound results the session's turns stored, each with
 * the turn that stored it (0: an ended turn's, in the live history), kept
 * like the facts a turn saves before it has a conversation (history_mutex).
 * ------------------------------------------------------------------------- */

typedef struct {
   char id[TOOL_RESULTS_ID_LEN];
   uint64_t turn_token;
} minted_t;

struct tool_result_minted {
   minted_t *items;
   int count;
   int cap;
};

/* The running turn's token when the caller is that turn (its thread, or a
 * tool thread carrying its token), else 0. */
static uint64_t caller_turn_locked(const session_t *session) {
   const uint64_t token = session_turn_token();
   return session->turn_active && token != 0 && token == session->turn_owner_token ? token : 0;
}

/* Whether the caller's context is the live history (not a turn running on
 * its own copy of another conversation). */
static bool caller_on_live_locked(const session_t *session) {
   return caller_turn_locked(session) == 0 || !session->turn_history ||
          session->turn_history == session->conversation_history;
}

static const minted_t *minted_find_locked(const session_t *session, const char *id) {
   const struct tool_result_minted *m = session->tool_results_minted;
   for (int i = 0; m && i < m->count; i++) {
      if (strcmp(m->items[i].id, id) == 0) {
         return &m->items[i];
      }
   }
   return NULL;
}

/* Whether the caller may read unbound result @p id: its own turn stored it,
 * or an ended turn did into the live history the caller is on. */
static bool minted_readable_locked(const session_t *session, const char *id) {
   const minted_t *e = minted_find_locked(session, id);
   if (!e) {
      return false;
   }
   const uint64_t caller = caller_turn_locked(session);
   return e->turn_token != 0 ? e->turn_token == caller : caller_on_live_locked(session);
}

static bool minted_add_locked(session_t *session, const char *id, uint64_t token) {
   struct tool_result_minted *m = session->tool_results_minted;
   if (!m) {
      m = calloc(1, sizeof(*m));
      if (!m) {
         return false;
      }
      session->tool_results_minted = m;
   }
   if (m->count == m->cap) {
      const int cap = m->cap ? m->cap * 2 : 8;
      minted_t *items = realloc(m->items, (size_t)cap * sizeof(*items));
      if (!items) {
         return false;
      }
      m->items = items;
      m->cap = cap;
   }
   memcpy(m->items[m->count].id, id, TOOL_RESULTS_ID_LEN);
   m->items[m->count++].turn_token = token;
   return true;
}

/* Remove the entries @p match selects (@p token, and the ended turns' when
 * @p ended). */
static bool minted_selects(const minted_t *e, uint64_t token, bool ended) {
   return (token != 0 && e->turn_token == token) || (ended && e->turn_token == 0);
}

static void minted_remove_locked(session_t *session, uint64_t token, bool ended) {
   struct tool_result_minted *m = session->tool_results_minted;
   int keep = 0;
   for (int i = 0; m && i < m->count; i++) {
      if (!minted_selects(&m->items[i], token, ended)) {
         m->items[keep++] = m->items[i];
      }
   }
   if (m) {
      m->count = keep;
   }
}

static void minted_free(struct tool_result_minted *m) {
   if (m) {
      free(m->items);
      free(m);
   }
}

/* ---------------------------------------------------------------------------
 * The tree cache.  s_cache_mutex guards only lookup and bookkeeping (a leaf
 * lock, never held across a render): a slot in use is pinned (busy) and used
 * by its one holder, lock-free; dropping or evicting never touches a busy
 * slot (a busy dropped slot is freed at its release).
 * ------------------------------------------------------------------------- */

#define TREE_CACHE_SLOTS 32
/* The last slot holds one tree of a result too large for the others (up to
 * LLM_TOOL_VIEW_JSON_MAX_BYTES of JSON), outside the cache's byte cap: its
 * turn's follow-ups read it without parsing it again. */
#define TREE_CACHE_LARGE (TREE_CACHE_SLOTS - 1)

typedef struct {
   const session_t *owner; /* NULL: free */
   char id[TOOL_RESULTS_ID_LEN];
   struct json_object *tree;
   size_t est;
   uint64_t used;
   bool busy;   /* pinned by a reader */
   bool doomed; /* dropped while busy: freed at release, never found */
} tree_slot_t;

static pthread_mutex_t s_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static tree_slot_t s_cache[TREE_CACHE_SLOTS];
static size_t s_cache_bytes;
static uint64_t s_cache_clock;

/* One tree of a result too large to cache is parsed and used at a time (a
 * json-c tree takes ~TOOL_RESULT_TREE_BYTES_PER_TEXT_BYTE times its text:
 * parallel reads of a 4 MB result would take hundreds of MB). */
static pthread_mutex_t s_big_parse_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Trees taken out of the cache under the lock, freed after it's released (a
 * large tree takes milliseconds to free). */
typedef struct {
   struct json_object *trees[TREE_CACHE_SLOTS + 1];
   int count;
} freed_t;

static void slot_drop_locked(tree_slot_t *slot, freed_t *freed) {
   if (freed->count < (int)(sizeof(freed->trees) / sizeof(freed->trees[0]))) {
      freed->trees[freed->count++] = slot->tree;
   } else {
      json_object_put(slot->tree);
   }
   if (slot != &s_cache[TREE_CACHE_LARGE]) {
      s_cache_bytes -= slot->est;
   }
   memset(slot, 0, sizeof(*slot));
}

static void freed_release(freed_t *freed) {
   for (int i = 0; i < freed->count; i++) {
      json_object_put(freed->trees[i]);
   }
   freed->count = 0;
}

static tree_slot_t *slot_find_locked(const session_t *session, const char *id) {
   for (int i = 0; i < TREE_CACHE_SLOTS; i++) {
      tree_slot_t *slot = &s_cache[i];
      if (slot->owner == session && !slot->doomed && strcmp(slot->id, id) == 0) {
         return slot;
      }
   }
   return NULL;
}

static size_t tree_estimate(size_t text_bytes) {
   return text_bytes * TOOL_RESULT_TREE_BYTES_PER_TEXT_BYTE;
}

/* A free slot with room for a tree of @p est bytes for @p session, evicting
 * its own oldest past the per-session count, then anyone's oldest past the
 * total (never a busy slot): NULL when that can't be done. */
static tree_slot_t *slot_make_room_locked(const session_t *session, size_t est, freed_t *freed) {
   if (est > tree_estimate(TOOL_RESULT_TREE_CACHE_TEXT_MAX)) {
      /* The large slot: its tree goes when another large one comes, unless
       * it's in use. */
      tree_slot_t *large = &s_cache[TREE_CACHE_LARGE];
      if (large->owner && large->busy) {
         return NULL;
      }
      if (large->owner) {
         slot_drop_locked(large, freed);
      }
      return large;
   }
   for (;;) {
      int own = 0;
      tree_slot_t *own_oldest = NULL;
      tree_slot_t *oldest = NULL;
      tree_slot_t *empty = NULL;
      for (int i = 0; i < TREE_CACHE_LARGE; i++) {
         tree_slot_t *slot = &s_cache[i];
         if (!slot->owner) {
            empty = empty ? empty : slot;
            continue;
         }
         if (slot->owner == session && !slot->doomed) {
            own++;
            if (!slot->busy && (!own_oldest || slot->used < own_oldest->used)) {
               own_oldest = slot;
            }
         }
         if (!slot->busy && (!oldest || slot->used < oldest->used)) {
            oldest = slot;
         }
      }
      if (own >= TOOL_RESULT_TREE_CACHE_PER_SESSION) {
         if (!own_oldest) {
            return NULL;
         }
         slot_drop_locked(own_oldest, freed);
      } else if (s_cache_bytes + est > TOOL_RESULT_TREE_CACHE_MAX_BYTES || !empty) {
         if (!oldest) {
            return NULL;
         }
         slot_drop_locked(oldest, freed);
      } else {
         return empty;
      }
   }
}

/* Put @p tree in a slot for @p session / @p id, pinned when @p pin: the slot,
 * or NULL (no room; the caller still owns @p tree). */
static tree_slot_t *slot_insert_locked(const session_t *session,
                                       const char *id,
                                       struct json_object *tree,
                                       size_t text_bytes,
                                       bool pin,
                                       freed_t *freed) {
   const size_t est = tree_estimate(text_bytes);
   tree_slot_t *slot = slot_make_room_locked(session, est, freed);
   if (!slot) {
      return NULL;
   }
   slot->owner = session;
   memcpy(slot->id, id, TOOL_RESULTS_ID_LEN);
   slot->tree = tree;
   slot->est = est;
   slot->used = ++s_cache_clock;
   slot->busy = pin;
   if (slot != &s_cache[TREE_CACHE_LARGE]) {
      s_cache_bytes += est;
   }
   return slot;
}

void tool_result_store_tree_seed(session_t *session,
                                 const char *id,
                                 struct json_object *tree,
                                 size_t text_bytes) {
   if (!session || !id || !tree || text_bytes > LLM_TOOL_VIEW_JSON_MAX_BYTES) {
      json_object_put(tree);
      return;
   }
   freed_t freed = { .count = 0 };
   pthread_mutex_lock(&s_cache_mutex);
   tree_slot_t *had = slot_find_locked(session, id);
   bool cached = false;
   if (!had || !had->busy) { /* in use: the cached tree stands */
      if (had) {
         slot_drop_locked(had, &freed);
      }
      cached = slot_insert_locked(session, id, tree, text_bytes, false, &freed) != NULL;
   }
   pthread_mutex_unlock(&s_cache_mutex);
   freed_release(&freed);
   if (!cached) {
      json_object_put(tree);
   }
}

bool tool_result_store_tree_acquire(session_t *session,
                                    tool_result_doc_t *doc,
                                    tool_result_tree_t *ref) {
   memset(ref, 0, sizeof(*ref));
   if (!doc || doc->meta.kind != TOOL_RESULTS_JSON) {
      return false;
   }
   if (session) {
      pthread_mutex_lock(&s_cache_mutex);
      tree_slot_t *slot = slot_find_locked(session, doc->id);
      if (slot && !slot->busy) {
         slot->busy = true;
         slot->used = ++s_cache_clock;
         ref->tree = slot->tree;
         ref->slot = slot;
         pthread_mutex_unlock(&s_cache_mutex);
         return true;
      }
      pthread_mutex_unlock(&s_cache_mutex);
   }
   /* Not cached (or in use by another reader): parse it, without the lock. */
   if (tool_result_store_load_body(doc) != TOOL_RESULT_OPEN_OK) {
      return false;
   }
   const bool large = doc->len > TOOL_RESULT_TREE_CACHE_TEXT_MAX;
   if (large) {
      /* One large parse at a time (see s_big_parse_mutex). */
      pthread_mutex_lock(&s_big_parse_mutex);
   }
   struct json_object *tree = llm_tool_view_parse(doc->body, doc->len);
   if (!tree) {
      if (large) {
         pthread_mutex_unlock(&s_big_parse_mutex);
      }
      return false;
   }
   tree_slot_t *slot = NULL;
   freed_t freed = { .count = 0 };
   if (session) {
      pthread_mutex_lock(&s_cache_mutex);
      slot = slot_find_locked(session, doc->id)
                 ? NULL
                 : slot_insert_locked(session, doc->id, tree, doc->len, true, &freed);
      pthread_mutex_unlock(&s_cache_mutex);
      freed_release(&freed);
   }
   ref->tree = tree;
   ref->slot = slot;
   ref->owned = slot == NULL; /* another reader's copy is cached, or no room */
   /* A large tree in the cache is pinned like any other; one parsed for this
    * read alone keeps the one-at-a-time hold until released. */
   ref->big = large && slot == NULL;
   if (large && slot) {
      pthread_mutex_unlock(&s_big_parse_mutex);
   }
   return true;
}

void tool_result_store_tree_release(tool_result_tree_t *ref) {
   if (!ref) {
      return;
   }
   if (ref->slot) {
      pthread_mutex_lock(&s_cache_mutex);
      tree_slot_t *slot = ref->slot;
      freed_t freed = { .count = 0 };
      slot->busy = false;
      if (slot->doomed) {
         slot_drop_locked(slot, &freed);
      }
      pthread_mutex_unlock(&s_cache_mutex);
      freed_release(&freed);
   } else if (ref->owned) {
      json_object_put(ref->tree);
   }
   if (ref->big) {
      pthread_mutex_unlock(&s_big_parse_mutex);
   }
   memset(ref, 0, sizeof(*ref));
}

void tool_result_store_drop_trees(session_t *session) {
   if (!session) {
      return;
   }
   freed_t freed = { .count = 0 };
   pthread_mutex_lock(&s_cache_mutex);
   for (int i = 0; i < TREE_CACHE_SLOTS; i++) {
      tree_slot_t *slot = &s_cache[i];
      if (slot->owner != session) {
         continue;
      }
      if (slot->busy) {
         slot->doomed = true;
      } else {
         slot_drop_locked(slot, &freed);
      }
   }
   pthread_mutex_unlock(&s_cache_mutex);
   freed_release(&freed);
}

/* ---------------------------------------------------------------------------
 * Storing
 * ------------------------------------------------------------------------- */

static uint32_t s_boot_nonce;
static pthread_once_t s_boot_once = PTHREAD_ONCE_INIT;

static void boot_nonce_init(void) {
   if (getrandom(&s_boot_nonce, sizeof(s_boot_nonce), 0) != (ssize_t)sizeof(s_boot_nonce)) {
      s_boot_nonce = (uint32_t)time(NULL);
   }
}

/* A key naming the storing session while its results are unbound: this run
 * of the daemon and the session's id (for the row and the log; the minted set
 * decides who may read). */
static void session_key(const session_t *session, char out[32]) {
   (void)pthread_once(&s_boot_once, boot_nonce_init);
   snprintf(out, 32, "%08x.%u", s_boot_nonce, session ? session->session_id : 0U);
}

static int64_t utf8_chars(const char *s, size_t n) {
   int64_t chars = 0;
   for (size_t i = 0; i < n; i++) {
      chars += ((unsigned char)s[i] & 0xC0) != 0x80;
   }
   return chars;
}

/* A NUL byte as a space, as the view shows it: a stored body is a C string,
 * the text its view was made from. */
static void nul_to_space(char *s, size_t n) {
   for (char *p = s; (p = memchr(p, '\0', (size_t)(s + n - p))) != NULL;) {
      *p = ' ';
   }
}

/* @p text as stored (heap, *@p out_len bytes): whole, or past
 * TOOL_RESULT_STORE_MAX_BYTES its head and tail (each cut at a line, else a
 * character boundary) with what was left out between them. */
static char *stored_copy(const char *text, size_t len, size_t *out_len) {
   if (len <= TOOL_RESULT_STORE_MAX_BYTES) {
      char *out = malloc(len + 1);
      if (out) {
         memcpy(out, text, len);
         out[len] = '\0';
         *out_len = len;
         nul_to_space(out, len);
      }
      return out;
   }
   const size_t keep = TOOL_RESULT_STORE_MAX_BYTES / 2 - 256;
   size_t head = keep;
   const char *nl = memrchr(text, '\n', head);
   if (nl && (size_t)(nl - text) > keep / 2) {
      head = (size_t)(nl - text) + 1;
   }
   while (head > 0 && ((unsigned char)text[head] & 0xC0) == 0x80) {
      head--;
   }
   size_t tail = len - keep;
   const char *nl2 = memchr(text + tail, '\n', keep / 2);
   if (nl2) {
      tail = (size_t)(nl2 - text) + 1;
   }
   while (tail < len && ((unsigned char)text[tail] & 0xC0) == 0x80) {
      tail++;
   }
   char mark[160];
   const int m = snprintf(mark, sizeof(mark),
                          "\n... %zu bytes omitted: the result was larger than DAWN keeps ...\n",
                          tail - head);
   char *out = malloc(head + (size_t)m + (len - tail) + 1);
   if (!out) {
      return NULL;
   }
   memcpy(out, text, head);
   memcpy(out + head, mark, (size_t)m);
   memcpy(out + head + (size_t)m, text + tail, len - tail);
   *out_len = head + (size_t)m + (len - tail);
   out[*out_len] = '\0';
   nul_to_space(out, *out_len);
   return out;
}

/* Caller holds history_mutex.  The caller's context: its turn token (0: no
 * turn) and conversation (its turn's, else the live history's; 0: none yet).
 * False for a thread that isn't the running turn's (a cancelled turn's tool
 * still running): it stores and reads nothing. */
static bool caller_context_locked(const session_t *session, uint64_t *token, int64_t *conv) {
   const uint64_t turn = caller_turn_locked(session);
   if (session->turn_active && turn == 0) {
      return false;
   }
   const int64_t c = turn ? session->turn_history_conv
                          : atomic_load(&session->history_conversation_id);
   *token = turn;
   *conv = c > 0 ? c : 0;
   return true;
}

int tool_result_store_put(session_t *session,
                          int user_id,
                          const char *tool_name,
                          const char *tool_call_id,
                          const char *text,
                          size_t len,
                          bool is_json,
                          char id_out[TOOL_RESULTS_ID_LEN],
                          bool *cut_out) {
   if (cut_out) {
      *cut_out = false;
   }
   if (id_out) {
      id_out[0] = '\0';
   }
   if (!session || user_id <= 0) {
      return TOOL_RESULT_STORE_NO_USER;
   }
   if (!tool_name || !text || !id_out) {
      return TOOL_RESULT_STORE_FAILED;
   }
   uint64_t token = 0;
   int64_t conv = 0;
   pthread_mutex_lock(&session->history_mutex);
   const bool may = caller_context_locked(session, &token, &conv);
   const uint64_t live_gen = session->tool_results_live_gen;
   pthread_mutex_unlock(&session->history_mutex);
   if (!may) {
      return TOOL_RESULT_STORE_FAILED;
   }
   char id[TOOL_RESULTS_ID_LEN];
   if (blob_generate_id(TOOL_RESULTS_ID_PREFIX, id) != BLOB_STORE_SUCCESS) {
      return TOOL_RESULT_STORE_FAILED;
   }
   const bool too_big = len > TOOL_RESULT_STORE_MAX_BYTES;
   size_t bytes = 0;
   char *body = stored_copy(text, len, &bytes);
   /* Never the conversation's tag on disk. */
   body = body ? session_prefix_mask_secret(session, body) : NULL;
   if (!body) {
      return TOOL_RESULT_STORE_FAILED;
   }
   bytes = strlen(body); /* the mask may change it */
   char key[32];
   session_key(session, key);
   const tool_results_new_t row = {
      .id = id,
      .user_id = user_id,
      .conversation_id = conv,
      .session_key = key,
      .tool_name = tool_name,
      .tool_call_id = tool_call_id,
      .kind = is_json && !too_big ? TOOL_RESULTS_JSON : TOOL_RESULTS_TEXT,
      .chars = utf8_chars(body, bytes),
      .body = body,
      .bytes = bytes,
   };
   int evicted = 0;
   const int rc = tool_results_db_add(&row, &evicted);
   free(body);
   if (rc != AUTH_DB_SUCCESS) {
      OLOG_ERROR("tool_result_store: %s's result (%zu bytes) not stored (%d)", tool_name, len, rc);
      return TOOL_RESULT_STORE_FAILED;
   }
   if (conv == 0) {
      /* Readable by its turn until its conversation exists; bound now if the
       * conversation came to be while it was being stored (the binds that
       * ran meanwhile didn't list it). */
      pthread_mutex_lock(&session->history_mutex);
      uint64_t now_token = 0;
      int64_t now_conv = 0;
      /* Still the same context: the same turn, or with no turn the same live
       * history (not replaced or started over meanwhile). */
      bool kept = caller_context_locked(session, &now_token, &now_conv) && now_token == token &&
                  (token != 0 || session->tool_results_live_gen == live_gen);
      if (kept && now_conv > 0) {
         const char(*one)[TOOL_RESULTS_ID_LEN] = (const char(*)[TOOL_RESULTS_ID_LEN])id;
         kept = tool_results_db_bind(now_conv, one, 1) == AUTH_DB_SUCCESS;
      } else if (kept) {
         kept = minted_add_locked(session, id, token);
      }
      pthread_mutex_unlock(&session->history_mutex);
      if (!kept) {
         (void)tool_results_db_delete(id); /* a handle no one could read */
         return TOOL_RESULT_STORE_FAILED;
      }
   }
   memcpy(id_out, id, TOOL_RESULTS_ID_LEN);
   if (cut_out) {
      *cut_out = too_big;
   }
   return TOOL_RESULT_STORE_OK;
}

/* ---------------------------------------------------------------------------
 * Reading
 * ------------------------------------------------------------------------- */

int tool_result_store_open(session_t *session,
                           int user_id,
                           const char *id,
                           tool_result_doc_t *doc) {
   if (!doc) {
      return TOOL_RESULT_OPEN_FAILED;
   }
   memset(doc, 0, sizeof(*doc));
   if (!session || user_id <= 0 || !tool_results_id_valid(id)) {
      return TOOL_RESULT_OPEN_REFUSED;
   }
   uint64_t token = 0;
   int64_t conv = 0;
   /* The row is read under the same hold as the caller's context: a bind
    * (which runs under this lock) can't fall between the two. */
   pthread_mutex_lock(&session->history_mutex);
   const bool may = caller_context_locked(session, &token, &conv);
   const bool minted = may && minted_readable_locked(session, id);
   const bool found = may && tool_results_db_get(id, &doc->meta, NULL, NULL) == AUTH_DB_SUCCESS;
   pthread_mutex_unlock(&session->history_mutex);
   if (!found) {
      memset(doc, 0, sizeof(*doc));
      return TOOL_RESULT_OPEN_REFUSED;
   }
   /* The user's own, and in the caller's conversation; or, not bound to any
    * yet, stored by the caller's turn (or an ended one of the history it's
    * on). */
   const bool readable = doc->meta.user_id == user_id &&
                         (doc->meta.conversation_id > 0
                              ? conv > 0 && doc->meta.conversation_id == conv
                              : minted);
   if (!readable) {
      memset(doc, 0, sizeof(*doc));
      return TOOL_RESULT_OPEN_REFUSED;
   }
   memcpy(doc->id, id, TOOL_RESULTS_ID_LEN);
   return TOOL_RESULT_OPEN_OK;
}

int tool_result_store_load_body(tool_result_doc_t *doc) {
   if (!doc || !doc->id[0]) {
      return TOOL_RESULT_OPEN_REFUSED;
   }
   if (doc->body) {
      return TOOL_RESULT_OPEN_OK;
   }
   tool_results_meta_t meta;
   const int rc = tool_results_db_get(doc->id, &meta, &doc->body, &doc->len);
   if (rc == AUTH_DB_NOT_FOUND) {
      return TOOL_RESULT_OPEN_REFUSED; /* evicted since it was opened */
   }
   /* Still the row that was allowed: the same user, and the same
    * conversation, or bound since to the one its history became (a bind only
    * ever gives an unbound row a conversation). */
   const bool same = rc == AUTH_DB_SUCCESS && meta.user_id == doc->meta.user_id &&
                     (meta.conversation_id == doc->meta.conversation_id ||
                      doc->meta.conversation_id == 0);
   if (!same) {
      free(doc->body);
      doc->body = NULL;
      doc->len = 0;
      return rc == AUTH_DB_SUCCESS ? TOOL_RESULT_OPEN_REFUSED : TOOL_RESULT_OPEN_FAILED;
   }
   return TOOL_RESULT_OPEN_OK;
}

void tool_result_store_close(tool_result_doc_t *doc) {
   if (doc) {
      free(doc->body);
      memset(doc, 0, sizeof(*doc));
   }
}

/* ---------------------------------------------------------------------------
 * A session's history: binding, a turn's end, starting over, freeing
 * ------------------------------------------------------------------------- */

int tool_result_store_bind_locked(session_t *session,
                                  int64_t conv_id,
                                  uint64_t turn_token,
                                  bool with_ended) {
   struct tool_result_minted *m = session ? session->tool_results_minted : NULL;
   if (!m || m->count == 0 || conv_id <= 0) {
      return AUTH_DB_SUCCESS;
   }
   char(*ids)[TOOL_RESULTS_ID_LEN] = malloc((size_t)m->count * sizeof(*ids));
   if (!ids) {
      return AUTH_DB_FAILURE;
   }
   int n = 0;
   for (int i = 0; i < m->count; i++) {
      if (minted_selects(&m->items[i], turn_token, with_ended)) {
         memcpy(ids[n++], m->items[i].id, TOOL_RESULTS_ID_LEN);
      }
   }
   const int rc = n > 0 ? tool_results_db_bind(conv_id, (const char(*)[TOOL_RESULTS_ID_LEN])ids, n)
                        : AUTH_DB_SUCCESS;
   free(ids);
   if (rc == AUTH_DB_SUCCESS) {
      /* Bound: read through their conversation from now on. */
      minted_remove_locked(session, turn_token, with_ended);
   }
   return rc;
}

void tool_result_store_turn_ended_locked(session_t *session, uint64_t turn_token, bool to_live) {
   struct tool_result_minted *m = session ? session->tool_results_minted : NULL;
   if (!m || turn_token == 0) {
      return;
   }
   if (!to_live) {
      /* It never got a conversation, and didn't write the live history: no
       * one will. */
      minted_remove_locked(session, turn_token, false);
      return;
   }
   for (int i = 0; i < m->count; i++) {
      if (m->items[i].turn_token == turn_token) {
         m->items[i].turn_token = 0; /* the live history's now */
      }
   }
}

void tool_result_store_reset_locked(session_t *session) {
   if (session) {
      minted_remove_locked(session, 0, true); /* a running turn's stay with it */
      session->tool_results_live_gen++;       /* a result stored for it now is no one's */
   }
}

void tool_result_store_free(session_t *session) {
   if (!session) {
      return;
   }
   minted_free(session->tool_results_minted);
   session->tool_results_minted = NULL;
   tool_result_store_drop_trees(session);
}

size_t tool_result_store_read_budget(session_t *session) {
   (void)session;
   return TOOL_RESULT_READ_BUDGET_CHARS;
}
