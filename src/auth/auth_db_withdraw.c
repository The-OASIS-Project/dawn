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
 * Withdrawing what a user forgot or deleted from their conversations' stored
 * request context (auth_db_withdraw.h), against the withdrawn_items rows
 * (made by the v94 migration) that this file's TEMP delete triggers leave
 * while a removal is marked (auth_db_withdraw_install).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "auth/auth_db_withdraw.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "llm/llm_context_text.h"
#include "logging.h"

/** How long a withdrawn item's row is kept: a turn built before the item went
 *  and saved after it, or a history saved long after (a voice session), is
 *  checked against it within this. */
#define WITHDRAWN_ITEMS_KEEP_SEC (7 * 24 * 3600)

/** How long before a turn was built an item's removal can still be in it
 *  (retrieval caches). */
#define WITHDRAW_BUILT_GRACE_SEC 60

/** The item_id of a user's USER MEMORY block, withdrawn as one. */
#define WITHDRAWN_MEMORY "memory"

/** The item_id of a withdrawal's own row: its id is where the withdrawal
 *  falls in the sequence the rows' ids make (conv_db_withdraw_seq), which is
 *  how a turn is known to have been built before or after it.  An item's id
 *  has a ':' ("fact:12"); these two don't. */
#define WITHDRAWN_MARK "withdrawal"

/* The user whose removal this thread is making (0: none): what the TEMP
 * delete triggers record a deleted item for. */
static __thread int tl_intent_user;

void conv_db_withdraw_intent_begin(int user_id) {
   tl_intent_user = user_id > 0 ? user_id : 0;
}

void conv_db_withdraw_intent_end(void) {
   tl_intent_user = 0;
}

static void intent_fn(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
   (void)argc;
   (void)argv;
   sqlite3_result_int(ctx, tl_intent_user);
}

int auth_db_withdraw_install(sqlite3 *db) {
   if (sqlite3_create_function(db, "dawn_withdraw_intent", 0, SQLITE_UTF8, NULL, intent_fn, NULL,
                               NULL) != SQLITE_OK) {
      OLOG_ERROR("withdraw: can't register its SQL function: %s", sqlite3_errmsg(db));
      return AUTH_DB_FAILURE;
   }
   /* TEMP: on this connection only, so the sqlite3 shell's deletes (which has
    * no such function) run as they always did.  A deleted item is recorded
    * only while a removal is marked: nightly decay, merges and re-indexing
    * are not the user forgetting anything. */
#define WITHDRAW_TRIGGER(name, table, prefix, owner)                                       \
   "CREATE TEMP TRIGGER IF NOT EXISTS " name " AFTER DELETE ON main." table                \
   " WHEN dawn_withdraw_intent() > 0 BEGIN INSERT INTO withdrawn_items (item_id, user_id)" \
   " VALUES ('" prefix ":' || OLD.id, " owner "); END;"
   static const char sql[] = WITHDRAW_TRIGGER("withdraw_fact", "memory_facts", "fact",
                                              "dawn_withdraw_intent()")
       WITHDRAW_TRIGGER("withdraw_summary", "memory_summaries", "summary", "dawn_withdraw_intent()")
           WITHDRAW_TRIGGER("withdraw_entity", "memory_entities", "entity",
                            "dawn_withdraw_intent()")
               WITHDRAW_TRIGGER("withdraw_relation", "memory_relations", "relation",
                                "dawn_withdraw_intent()")
                   WITHDRAW_TRIGGER("withdraw_document_chunk", "document_chunks", "document_chunk",
                                    "dawn_withdraw_intent()");
#undef WITHDRAW_TRIGGER
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      /* A user's forgetting would silently stop reaching stored conversations:
       * refused, not degraded. */
      OLOG_ERROR("withdraw: can't install its delete triggers: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

void conv_withdrawn_free(conv_withdrawn_t *w) {
   if (!w) {
      return;
   }
   free(w->items);
   free(w->convs);
   for (int i = 0; i < w->n_item_ids; i++) {
      free(w->item_ids[i]);
   }
   free(w->item_ids);
   memset(w, 0, sizeof(*w));
}

/* Grow @p *arr (of @p size-byte entries, @p *cap of them) to hold @p need. */
static bool grow(void **arr, int *cap, int need, size_t size) {
   if (need <= *cap) {
      return true;
   }
   int next = *cap ? *cap * 2 : 16;
   while (next < need) {
      next *= 2;
   }
   void *grown = realloc(*arr, (size_t)next * size);
   if (!grown) {
      return false;
   }
   *arr = grown;
   *cap = next;
   return true;
}

/* Statements a withdrawal uses, prepared once. */
typedef struct {
   sqlite3_stmt *rows;   /* a conversation's turn contexts from an id */
   sqlite3_stmt *update; /* a row's new content */
   sqlite3_stmt *mark;   /* a handle withdrawn */
} withdraw_stmts_t;

static void stmts_finalize(withdraw_stmts_t *st) {
   sqlite3_finalize(st->rows);
   sqlite3_finalize(st->update);
   sqlite3_finalize(st->mark);
   memset(st, 0, sizeof(*st));
}

static bool stmts_prepare(withdraw_stmts_t *st) {
   memset(st, 0, sizeof(*st));
   /* kind-rows: an item's line lives only in stored turn contexts. */
   const bool ok = sqlite3_prepare_v2(
                       s_db.db,
                       "SELECT id, content FROM messages WHERE kind = 'turn_context' AND "
                       "conversation_id = ?1 AND id >= ?2 AND instr(content, '[M') > 0",
                       -1, &st->rows, NULL) == SQLITE_OK &&
                   sqlite3_prepare_v2(s_db.db, "UPDATE messages SET content = ? WHERE id = ?", -1,
                                      &st->update, NULL) == SQLITE_OK &&
                   sqlite3_prepare_v2(s_db.db,
                                      "UPDATE conversation_focus_handles SET withdrawn = 1 "
                                      "WHERE conversation_id = ? AND handle = ?",
                                      -1, &st->mark, NULL) == SQLITE_OK;
   if (!ok) {
      stmts_finalize(st);
   }
   return ok;
}

/* Withdraw @p handles' lines from conversation @p conv's turn contexts saved
 * at or after row @p from_id, and mark them withdrawn.  Caller holds the lock
 * and the transaction.  Returns false on failure; @p changed says whether a
 * row changed. */
static bool withdraw_handles_locked(withdraw_stmts_t *st,
                                    int64_t conv,
                                    int64_t from_id,
                                    const int *handles,
                                    int count,
                                    bool *changed) {
   *changed = false;
   sqlite3_reset(st->rows);
   sqlite3_bind_int64(st->rows, 1, conv);
   sqlite3_bind_int64(st->rows, 2, from_id);
   bool ok = true;
   int rc;
   while (ok && (rc = sqlite3_step(st->rows)) == SQLITE_ROW) {
      char *text = NULL;
      if (llm_context_withdraw_items((const char *)sqlite3_column_text(st->rows, 1), handles, count,
                                     &text) != 0) {
         ok = false;
         break;
      }
      if (text) {
         sqlite3_reset(st->update);
         sqlite3_bind_text(st->update, 1, text, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int64(st->update, 2, sqlite3_column_int64(st->rows, 0));
         ok = sqlite3_step(st->update) == SQLITE_DONE;
         *changed = true;
         free(text);
      }
   }
   ok = ok && rc == SQLITE_DONE;
   for (int i = 0; ok && i < count; i++) {
      sqlite3_reset(st->mark);
      sqlite3_bind_int64(st->mark, 1, conv);
      sqlite3_bind_int(st->mark, 2, handles[i]);
      ok = sqlite3_step(st->mark) == SQLITE_DONE;
   }
   sqlite3_reset(st->rows);
   return ok;
}

/* Withdraw the USER MEMORY blocks @p select returns (id, content,
 * conversation_id; binding ?1 = @p key, ?2 = @p from_id), noting each
 * conversation changed in @p w (when given) and setting @p changed.  Caller
 * holds the lock and the transaction. */
static bool withdraw_bodies_locked(const char *select,
                                   int64_t key,
                                   int64_t from_id,
                                   conv_withdrawn_t *w,
                                   int *convs_cap,
                                   bool *changed) {
   sqlite3_stmt *st = NULL;
   sqlite3_stmt *up = NULL;
   bool ok = sqlite3_prepare_v2(s_db.db, select, -1, &st, NULL) == SQLITE_OK &&
             sqlite3_prepare_v2(s_db.db, "UPDATE messages SET content = ? WHERE id = ?", -1, &up,
                                NULL) == SQLITE_OK;
   if (ok) {
      sqlite3_bind_int64(st, 1, key);
      sqlite3_bind_int64(st, 2, from_id);
   }
   int rc = SQLITE_DONE;
   while (ok && (rc = sqlite3_step(st)) == SQLITE_ROW) {
      bool hit = false;
      char *text = llm_context_withdraw_body((const char *)sqlite3_column_text(st, 1), &hit);
      if (!text) {
         ok = false;
         break;
      }
      if (hit) {
         sqlite3_reset(up);
         sqlite3_bind_text(up, 1, text, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int64(up, 2, sqlite3_column_int64(st, 0));
         ok = sqlite3_step(up) == SQLITE_DONE;
         *changed = true;
         const int64_t conv = sqlite3_column_int64(st, 2);
         if (ok && w && (w->n_convs == 0 || w->convs[w->n_convs - 1] != conv)) {
            /* Rows come by conversation; repeats are dropped before floors rise. */
            ok = grow((void **)&w->convs, convs_cap, w->n_convs + 1, sizeof(*w->convs));
            if (ok) {
               w->convs[w->n_convs++] = conv;
            }
         }
      }
      free(text);
   }
   ok = ok && rc == SQLITE_DONE;
   sqlite3_finalize(st);
   sqlite3_finalize(up);
   return ok;
}

/* A withdrawal by @p user_id takes its place in the sequence (a row of its
 * own): its id, in @p seq_out.  Caller holds the lock and the transaction. */
static bool mark_locked(int user_id, int64_t *seq_out) {
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "INSERT INTO withdrawn_items (item_id, user_id, delivered) VALUES "
                          "('" WITHDRAWN_MARK "', ?, 1)",
                          -1, &st, NULL) != SQLITE_OK) {
      return false;
   }
   sqlite3_bind_int(st, 1, user_id);
   const bool ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   *seq_out = ok ? sqlite3_last_insert_rowid(s_db.db) : 0;
   return ok;
}

/* Conversation @p conv's stored context changed, in the withdrawal at @p seq:
 * every row it has leaves its reasoning behind now (the floor), and so does
 * any row saved before a turn built after the withdrawal is (pending: a reply
 * streamed during it).  Caller holds the lock and the transaction. */
static bool floor_pending_locked(int64_t conv, int64_t seq) {
   sqlite3_stmt *st = NULL;
   /* kind-rows: the floor is past every row the conversation has. */
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE conversations SET reasoning_floor_msg_id = "
                          "MAX(reasoning_floor_msg_id, (SELECT COALESCE(MAX(id), 0) FROM messages "
                          "WHERE conversation_id = ?1)), reasoning_floor_pending = ?2, "
                          "reasoning_floor_seq = MAX(reasoning_floor_seq, ?2) WHERE id = ?1",
                          -1, &st, NULL) != SQLITE_OK) {
      return false;
   }
   sqlite3_bind_int64(st, 1, conv);
   sqlite3_bind_int64(st, 2, seq);
   const bool ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   return ok;
}

int conv_db_withdraw_seq(int64_t *seq_out) {
   if (!seq_out) {
      return AUTH_DB_FAILURE;
   }
   *seq_out = 0;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int result = AUTH_DB_FAILURE;
   /* The AUTOINCREMENT high-water mark, not MAX(id): the purge can delete the
    * newest rows, and a floor pending on a purged marker must still settle. */
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT COALESCE((SELECT seq FROM sqlite_sequence "
                          "WHERE name = 'withdrawn_items'), 0)",
                          -1, &st, NULL) == SQLITE_OK &&
       sqlite3_step(st) == SQLITE_ROW) {
      *seq_out = sqlite3_column_int64(st, 0);
      result = AUTH_DB_SUCCESS;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return result;
}

/* The item ids the user's removals since the last withdrawal name, for live
 * sessions to check their own handles against. */
static bool live_item_ids_locked(int user_id, conv_withdrawn_t *w) {
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(
           s_db.db,
           "SELECT DISTINCT item_id FROM withdrawn_items WHERE user_id = ?1 AND delivered = 0 "
           "AND instr(item_id, ':') > 0 ORDER BY item_id",
           -1, &st, NULL) != SQLITE_OK) {
      return false;
   }
   sqlite3_bind_int(st, 1, user_id);
   int cap = 0;
   bool ok = true;
   int rc;
   while (ok && (rc = sqlite3_step(st)) == SQLITE_ROW) {
      ok = grow((void **)&w->item_ids, &cap, w->n_item_ids + 1, sizeof(*w->item_ids));
      char *id = ok ? strdup((const char *)sqlite3_column_text(st, 0)) : NULL;
      ok = ok && id;
      if (ok) {
         w->item_ids[w->n_item_ids++] = id;
      }
   }
   ok = ok && rc == SQLITE_DONE;
   sqlite3_finalize(st);
   return ok;
}

static int cmp_conv(const void *a, const void *b) {
   const int64_t x = *(const int64_t *)a;
   const int64_t y = *(const int64_t *)b;
   return (x > y) - (x < y);
}

int conv_db_withdraw(int user_id, bool memory_bodies, conv_withdrawn_t *out) {
   conv_withdrawn_t w = { 0 };
   int items_cap = 0;
   int convs_cap = 0;
   if (out) {
      memset(out, 0, sizeof(*out));
   }
   if (user_id <= 0) {
      return AUTH_DB_FAILURE;
   }
   const time_t now = time(NULL);
   AUTH_DB_LOCK_OR_FAIL();
   if (auth_db_txn_begin_locked("withdraw") != AUTH_DB_SUCCESS) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   withdraw_stmts_t st;
   bool ok = stmts_prepare(&st);

   /* Where each item the user removed since the last withdrawal was injected:
    * in any conversation that holds it (a shared document's chunks are in
    * other users' too), by conversation.  Driven from those rows into the
    * handles by item, never a scan of every handle. */
   sqlite3_stmt *where = NULL;
   ok = ok && sqlite3_prepare_v2(s_db.db,
                                 "SELECT DISTINCT h.conversation_id, h.handle "
                                 "FROM withdrawn_items t CROSS JOIN conversation_focus_handles h "
                                 "ON h.item_id = t.item_id "
                                 "WHERE t.user_id = ?1 AND t.delivered = 0 "
                                 "AND instr(t.item_id, ':') > 0 AND h.withdrawn = 0 "
                                 "ORDER BY h.conversation_id",
                                 -1, &where, NULL) == SQLITE_OK;
   if (ok) {
      sqlite3_bind_int(where, 1, user_id);
      int rc;
      while ((rc = sqlite3_step(where)) == SQLITE_ROW) {
         if (!grow((void **)&w.items, &items_cap, w.n_items + 1, sizeof(*w.items))) {
            ok = false;
            break;
         }
         w.items[w.n_items++] = (conv_withdrawn_item_t){ .conv_id = sqlite3_column_int64(where, 0),
                                                         .handle = sqlite3_column_int(where, 1) };
      }
      ok = ok && rc == SQLITE_DONE;
   }
   sqlite3_finalize(where);

   /* Each conversation's turn contexts rewritten once, for all its items. */
   int *handles = w.n_items ? malloc((size_t)w.n_items * sizeof(*handles)) : NULL;
   ok = ok && (w.n_items == 0 || handles);
   for (int i = 0; ok && i < w.n_items;) {
      const int64_t conv = w.items[i].conv_id;
      int n = 0;
      while (i < w.n_items && w.items[i].conv_id == conv) {
         handles[n++] = w.items[i++].handle;
      }
      bool changed = false;
      ok = withdraw_handles_locked(&st, conv, 0, handles, n, &changed);
      if (ok && changed) {
         ok = grow((void **)&w.convs, &convs_cap, w.n_convs + 1, sizeof(*w.convs));
         if (ok) {
            w.convs[w.n_convs++] = conv;
         }
      }
   }
   free(handles);

   /* Every stored USER MEMORY block since the last memory withdrawal covered
    * (those at or below its watermark were withdrawn then), and a row saying
    * when and how far (a turn built before it and saved after has its block
    * withdrawn then). */
   if (ok && memory_bodies) {
      int64_t from_id = 0;
      sqlite3_stmt *last = NULL;
      ok = sqlite3_prepare_v2(s_db.db,
                              "SELECT COALESCE(MAX(upto_msg_id), 0) FROM withdrawn_items "
                              "WHERE item_id = '" WITHDRAWN_MEMORY "' AND user_id = ?",
                              -1, &last, NULL) == SQLITE_OK;
      if (ok) {
         sqlite3_bind_int(last, 1, user_id);
         ok = sqlite3_step(last) == SQLITE_ROW;
         from_id = ok ? sqlite3_column_int64(last, 0) + 1 : 0;
      }
      sqlite3_finalize(last);
      bool changed = false;
      /* kind-rows: the stored USER MEMORY blocks. */
      ok = ok && withdraw_bodies_locked("SELECT m.id, m.content, m.conversation_id FROM messages m "
                                        "JOIN conversations c ON c.id = m.conversation_id "
                                        "WHERE m.kind = 'memory' AND c.user_id = ?1 AND m.id >= ?2 "
                                        "ORDER BY m.conversation_id",
                                        user_id, from_id, &w, &convs_cap, &changed);
      sqlite3_stmt *mark = NULL;
      /* kind-rows: the watermark is past every row, whatever its kind. */
      ok = ok && sqlite3_prepare_v2(s_db.db,
                                    "INSERT INTO withdrawn_items (item_id, user_id, created_at, "
                                    "delivered, upto_msg_id) VALUES ('" WITHDRAWN_MEMORY
                                    "', ?1, ?2, 1, (SELECT COALESCE(MAX(id), 0) FROM messages))",
                                    -1, &mark, NULL) == SQLITE_OK;
      if (ok) {
         sqlite3_bind_int(mark, 1, user_id);
         sqlite3_bind_int64(mark, 2, (int64_t)now);
         ok = sqlite3_step(mark) == SQLITE_DONE;
      }
      sqlite3_finalize(mark);
   }

   /* The conversations that changed (once each) leave their earlier
    * reasoning behind. */
   if (w.n_convs > 1) {
      qsort(w.convs, (size_t)w.n_convs, sizeof(*w.convs), cmp_conv);
      int kept = 1;
      for (int i = 1; i < w.n_convs; i++) {
         if (w.convs[i] != w.convs[kept - 1]) {
            w.convs[kept++] = w.convs[i];
         }
      }
      w.n_convs = kept;
   }
   int64_t seq = 0;
   ok = ok && (w.n_convs == 0 || mark_locked(user_id, &seq));
   for (int i = 0; ok && i < w.n_convs; i++) {
      ok = floor_pending_locked(w.convs[i], seq);
   }
   ok = ok && live_item_ids_locked(user_id, &w);
   if (ok) {
      sqlite3_stmt *sent = NULL;
      ok = sqlite3_prepare_v2(s_db.db,
                              "UPDATE withdrawn_items SET delivered = 1 WHERE user_id = ? AND "
                              "delivered = 0",
                              -1, &sent, NULL) == SQLITE_OK;
      if (ok) {
         sqlite3_bind_int(sent, 1, user_id);
         ok = sqlite3_step(sent) == SQLITE_DONE;
      }
      sqlite3_finalize(sent);
   }
   stmts_finalize(&st);

   ok = auth_db_txn_end_locked(ok ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE, "withdraw") ==
        AUTH_DB_SUCCESS;
   if (!ok) {
      OLOG_ERROR("withdraw: user %d's forgotten items not withdrawn: %s", user_id,
                 sqlite3_errmsg(s_db.db));
   }
   AUTH_DB_UNLOCK();
   if (ok && w.n_convs > 0) {
      OLOG_INFO("withdraw: user %d: %d item line(s)%s withdrawn from %d conversation(s)", user_id,
                w.n_items, memory_bodies ? " and memory blocks" : "", w.n_convs);
   }
   if (ok && out) {
      *out = w;
   } else {
      conv_withdrawn_free(&w);
   }
   return ok ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/* Whether a withdrawal later in the sequence than @p built_seq changed
 * conversation @p conv_id; if so, its floor goes past every row it has again,
 * pending on that withdrawal.  Caller holds the lock and the transaction. */
static bool stale_turn_locked(int64_t conv_id, int64_t built_seq, bool *stale_out) {
   *stale_out = false;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT reasoning_floor_seq FROM conversations WHERE id = ?", -1,
                          &st, NULL) != SQLITE_OK) {
      return false;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   const int rc = sqlite3_step(st);
   const int64_t floor_seq = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
   sqlite3_finalize(st);
   if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
      return false;
   }
   if (floor_seq <= built_seq) {
      return true;
   }
   *stale_out = true;
   return floor_pending_locked(conv_id, floor_seq);
}

int conv_db_withdraw_saved_locked(int64_t conv_id,
                                  int user_id,
                                  int64_t from_id,
                                  int64_t built_at,
                                  int64_t built_seq,
                                  bool *changed_out) {
   *changed_out = false;
   /* The conversation's handles for items since withdrawn: marked or not (a
    * handle is marked when the forgetting ran, maybe before this row existed). */
   sqlite3_stmt *gone = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT DISTINCT h.handle FROM conversation_focus_handles h "
                          "JOIN withdrawn_items t ON t.item_id = h.item_id "
                          "WHERE h.conversation_id = ?1 AND t.created_at >= ?2",
                          -1, &gone, NULL) != SQLITE_OK) {
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(gone, 1, conv_id);
   /* A turn built after an item was removed can't carry it (a minute's grace
    * for what retrieval had cached); 0: anything removed within the week. */
   sqlite3_bind_int64(gone, 2, built_at > 0 ? built_at - WITHDRAW_BUILT_GRACE_SEC : 0);
   int *handles = NULL;
   int n = 0;
   int cap = 0;
   bool ok = true;
   int rc;
   while ((rc = sqlite3_step(gone)) == SQLITE_ROW) {
      if (!grow((void **)&handles, &cap, n + 1, sizeof(*handles))) {
         ok = false;
         break;
      }
      handles[n++] = sqlite3_column_int(gone, 0);
   }
   ok = ok && rc == SQLITE_DONE;
   sqlite3_finalize(gone);

   bool changed = false;
   if (ok && n > 0) {
      withdraw_stmts_t st;
      ok = stmts_prepare(&st) &&
           withdraw_handles_locked(&st, conv_id, from_id, handles, n, &changed);
      stmts_finalize(&st);
   }
   free(handles);

   /* The user's memory, withdrawn after the turn was built (later in the
    * sequence: exact, where a clock can't order two things in one second;
    * a block built after it is the current one, and stays). */
   if (ok) {
      sqlite3_stmt *since = NULL;
      ok = sqlite3_prepare_v2(s_db.db,
                              "SELECT 1 FROM withdrawn_items WHERE item_id = '" WITHDRAWN_MEMORY
                              "' AND id > ? AND user_id = ? LIMIT 1",
                              -1, &since, NULL) == SQLITE_OK;
      bool withdrawn_since = false;
      if (ok) {
         sqlite3_bind_int64(since, 1, built_seq);
         sqlite3_bind_int(since, 2, user_id);
         const int step = sqlite3_step(since);
         withdrawn_since = step == SQLITE_ROW;
         ok = step == SQLITE_ROW || step == SQLITE_DONE;
      }
      sqlite3_finalize(since);
      bool hit = false;
      /* kind-rows: this conversation's USER MEMORY blocks saved just now. */
      ok = ok && (!withdrawn_since ||
                  withdraw_bodies_locked("SELECT id, content, conversation_id FROM messages "
                                         "WHERE kind = 'memory' AND conversation_id = ?1 "
                                         "AND id >= ?2",
                                         conv_id, from_id, NULL, NULL, &hit));
      changed = changed || hit;
   }
   int64_t seq = 0;
   if (ok && changed) {
      ok = mark_locked(user_id, &seq) && floor_pending_locked(conv_id, seq);
   } else if (ok) {
      /* A withdrawal since the turn was built may have scrubbed the history its
       * reasoning is bound to, and a turn built after it (another session's)
       * may have settled the floor already: this turn's rows go behind it
       * again, until a turn built after that withdrawal.  (A turn with no
       * build seq, its prompt unbuilt, counts as built before every one.) */
      ok = stale_turn_locked(conv_id, built_seq, &changed);
   }
   *changed_out = changed;
   return ok ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int conv_db_withdraw_conversation(int64_t conv_id, int user_id, int64_t since, int64_t since_seq) {
   if (conv_id <= 0 || user_id <= 0) {
      return AUTH_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_FAIL();
   if (auth_db_txn_begin_locked("withdraw") != AUTH_DB_SUCCESS) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   bool changed = false;
   int result = conv_db_owned_locked(conv_id, user_id);
   if (result == AUTH_DB_SUCCESS) {
      result = conv_db_withdraw_saved_locked(conv_id, user_id, 0, since, since_seq, &changed);
   }
   result = auth_db_txn_end_locked(result, "withdraw");
   AUTH_DB_UNLOCK();
   if (result == AUTH_DB_SUCCESS && changed) {
      OLOG_INFO("withdraw: conv %lld: saved context changed or predates a withdrawal",
                (long long)conv_id);
   }
   return result;
}

int conv_db_withdrawn_items_purge(int *deleted_out) {
   if (deleted_out) {
      *deleted_out = 0;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int result = AUTH_DB_FAILURE;
   /* Not one no withdrawal has delivered yet (its removal is still to be
    * withdrawn), nor a user's newest memory row (its watermark). */
   if (sqlite3_prepare_v2(
           s_db.db,
           "DELETE FROM withdrawn_items WHERE created_at < ? AND delivered = 1 "
           "AND id NOT IN (SELECT MAX(id) FROM withdrawn_items WHERE item_id = '" WITHDRAWN_MEMORY
           "' GROUP BY user_id)",
           -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, (int64_t)time(NULL) - WITHDRAWN_ITEMS_KEEP_SEC);
      if (sqlite3_step(st) == SQLITE_DONE) {
         result = AUTH_DB_SUCCESS;
         if (deleted_out) {
            *deleted_out = sqlite3_changes(s_db.db);
         }
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return result;
}
