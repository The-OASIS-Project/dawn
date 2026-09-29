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
 * Stable memory citation handles, per session (focus_handles.h).
 */

#include "core/focus/focus_handles.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "logging.h"

/* Caller holds history_mutex.  The table, created on first use. */
static focus_handles_t *table_locked(session_t *session) {
   if (!session->focus_handles) {
      session->focus_handles = calloc(1, sizeof(*session->focus_handles));
   }
   return session->focus_handles;
}

static focus_handle_t *find_item(focus_handles_t *t, const char *source, const char *item_id) {
   for (int i = 0; t && i < t->count; i++) {
      if (strcmp(t->items[i].source, source) == 0 && strcmp(t->items[i].item_id, item_id) == 0) {
         return &t->items[i];
      }
   }
   return NULL;
}

static bool has_handle(const focus_handles_t *t, int handle) {
   for (int i = 0; t && i < t->count; i++) {
      if (t->items[i].handle == handle) {
         return true;
      }
   }
   return false;
}

/* Add an entry; false on allocation failure. */
static bool add_item(focus_handles_t *t,
                     const char *source,
                     const char *item_id,
                     int handle,
                     bool saved) {
   if (t->count == t->cap) {
      const int cap = t->cap ? t->cap * 2 : 32;
      focus_handle_t *grown = realloc(t->items, (size_t)cap * sizeof(*grown));
      if (!grown) {
         return false;
      }
      t->items = grown;
      t->cap = cap;
   }
   focus_handle_t *e = &t->items[t->count++];
   snprintf(e->source, sizeof(e->source), "%s", source);
   snprintf(e->item_id, sizeof(e->item_id), "%s", item_id);
   e->handle = handle;
   e->saved = saved;
   return true;
}

/* Caller holds history_mutex.  Point the table at @p conv_id; with @p adopt, a
 * table of no conversation keeps its handles (its history has just become
 * @p conv_id).  Any other change of conversation starts over. */
static void retarget_locked(focus_handles_t *t, int64_t conv_id, bool adopt) {
   if (t->conv_id == conv_id) {
      return;
   }
   if (!(adopt && t->conv_id == 0 && conv_id > 0)) {
      t->count = 0;
   }
   t->conv_id = conv_id;
   t->loaded = conv_id == 0; /* nothing stored to read */
}

typedef struct {
   focus_handles_t *into;
} load_ctx_t;

static int on_stored(const char *source, const char *item_id, int handle, void *ctx) {
   focus_handles_t *t = ((load_ctx_t *)ctx)->into;
   if (!find_item(t, source, item_id) && !has_handle(t, handle)) {
      (void)add_item(t, source, item_id, handle, true);
   }
   return 0;
}

/* Read @p conv_id's stored handles into the table, once. */
static void load(session_t *session, int64_t conv_id, int user_id) {
   pthread_mutex_lock(&session->history_mutex);
   focus_handles_t *t = session->focus_handles;
   const bool need = t && t->conv_id == conv_id && !t->loaded;
   pthread_mutex_unlock(&session->history_mutex);
   if (!need) {
      return;
   }
   focus_handles_t stored = { 0 };
   load_ctx_t ctx = { .into = &stored };
   if (conv_db_focus_handles_load(conv_id, user_id, on_stored, &ctx) != AUTH_DB_SUCCESS) {
      free(stored.items);
      return; /* read again next time */
   }
   pthread_mutex_lock(&session->history_mutex);
   t = session->focus_handles;
   if (t && t->conv_id == conv_id && !t->loaded) {
      for (int i = 0; i < stored.count; i++) {
         const focus_handle_t *e = &stored.items[i];
         focus_handle_t *have = find_item(t, e->source, e->item_id);
         if (have) {
            have->handle = e->handle; /* the stored handle is the item's */
            have->saved = true;
         } else if (!has_handle(t, e->handle)) {
            (void)add_item(t, e->source, e->item_id, e->handle, true);
         }
      }
      t->loaded = true;
   }
   pthread_mutex_unlock(&session->history_mutex);
   free(stored.items);
}

int focus_handles_flush(session_t *session, int64_t conv_id, int user_id) {
   if (!session || conv_id <= 0) {
      return 0;
   }
   pthread_mutex_lock(&session->history_mutex);
   focus_handles_t *t = session->focus_handles;
   /* Handles numbered before the history had a conversation become this
    * conversation's only once the history is it. */
   const bool adopt = t && t->conv_id == 0 &&
                      atomic_load(&session->history_conversation_id) == conv_id;
   if (!t || !(adopt || t->conv_id == conv_id)) {
      pthread_mutex_unlock(&session->history_mutex);
      return 0;
   }
   retarget_locked(t, conv_id, true);
   if (user_id <= 0) {
      user_id = t->user_id;
   }
   int n = 0;
   for (int i = 0; i < t->count; i++) {
      n += !t->items[i].saved;
   }
   conv_focus_handle_t *put = n ? calloc((size_t)n, sizeof(*put)) : NULL;
   focus_handle_t *copies = n ? calloc((size_t)n, sizeof(*copies)) : NULL;
   int k = 0;
   for (int i = 0; put && copies && i < t->count; i++) {
      if (!t->items[i].saved) {
         copies[k] = t->items[i];
         put[k] = (conv_focus_handle_t){ .source = copies[k].source,
                                         .item_id = copies[k].item_id,
                                         .handle = copies[k].handle };
         k++;
      }
   }
   pthread_mutex_unlock(&session->history_mutex);
   if (n == 0) {
      return 0;
   }
   int rc = 1;
   if (put && copies && conv_db_focus_handles_put(conv_id, user_id, put, n) == AUTH_DB_SUCCESS) {
      rc = 0;
      pthread_mutex_lock(&session->history_mutex);
      t = session->focus_handles;
      for (int i = 0; t && t->conv_id == conv_id && i < n; i++) {
         focus_handle_t *e = find_item(t, copies[i].source, copies[i].item_id);
         if (e) {
            e->saved = true; /* stored, or the conversation already had it */
         }
      }
      pthread_mutex_unlock(&session->history_mutex);
   } else {
      OLOG_WARNING("focus_handles: session %u: %d handle(s) for conv %lld not saved",
                   session->session_id, n, (long long)conv_id);
   }
   free(put);
   free(copies);
   return rc;
}

int focus_handles_assign(session_t *session,
                         int64_t conv_id,
                         int user_id,
                         conv_focus_handle_t *items,
                         int count) {
   if (!session || !items || count <= 0) {
      return count > 0;
   }
   for (int i = 0; i < count; i++) {
      items[i].handle = 0;
      items[i].is_new = false;
   }
   pthread_mutex_lock(&session->history_mutex);
   focus_handles_t *t = table_locked(session);
   if (t) {
      retarget_locked(t, conv_id > 0 ? conv_id : 0, false);
      t->user_id = user_id;
   }
   pthread_mutex_unlock(&session->history_mutex);
   if (!t) {
      return 1;
   }
   if (conv_id > 0) {
      load(session, conv_id, user_id);
   }

   /* The items the conversation already has; the rest get the next handles. */
   int missing = 0;
   pthread_mutex_lock(&session->history_mutex);
   t = session->focus_handles;
   for (int i = 0; t && i < count; i++) {
      if (!items[i].source || !items[i].item_id) {
         continue;
      }
      const focus_handle_t *e = find_item(t, items[i].source, items[i].item_id);
      if (e) {
         items[i].handle = e->handle;
      } else if (conv_id <= 0) {
         int next = 1;
         for (int j = 0; j < t->count; j++) {
            next = t->items[j].handle >= next ? t->items[j].handle + 1 : next;
         }
         if (add_item(t, items[i].source, items[i].item_id, next, false)) {
            items[i].handle = next;
            items[i].is_new = true;
         }
      } else {
         missing++;
      }
   }
   pthread_mutex_unlock(&session->history_mutex);

   if (missing > 0) {
      conv_focus_handle_t *want = calloc((size_t)missing, sizeof(*want));
      int *slot = calloc((size_t)missing, sizeof(*slot));
      int k = 0;
      for (int i = 0; want && slot && i < count; i++) {
         if (items[i].handle == 0 && items[i].source && items[i].item_id) {
            want[k] = (conv_focus_handle_t){ .source = items[i].source,
                                             .item_id = items[i].item_id };
            slot[k++] = i;
         }
      }
      if (want && slot &&
          conv_db_focus_handles_assign(conv_id, user_id, want, k) == AUTH_DB_SUCCESS) {
         pthread_mutex_lock(&session->history_mutex);
         t = session->focus_handles;
         for (int j = 0; j < k; j++) {
            items[slot[j]].handle = want[j].handle;
            items[slot[j]].is_new = want[j].is_new;
            if (t && t->conv_id == conv_id && !find_item(t, want[j].source, want[j].item_id)) {
               (void)add_item(t, want[j].source, want[j].item_id, want[j].handle, true);
            }
         }
         pthread_mutex_unlock(&session->history_mutex);
      } else {
         OLOG_WARNING("focus_handles: session %u: conv %lld: %d item(s) got no handle",
                      session->session_id, (long long)conv_id, missing);
      }
      free(want);
      free(slot);
   }

   for (int i = 0; i < count; i++) {
      if (items[i].handle <= 0) {
         return 1;
      }
   }
   return 0;
}

bool focus_handles_item_locked(const session_t *session,
                               int handle,
                               char item_id[FOCUS_HANDLE_ITEM_ID_LEN]) {
   const focus_handles_t *t = session ? session->focus_handles : NULL;
   for (int i = 0; t && i < t->count; i++) {
      if (t->items[i].handle == handle) {
         snprintf(item_id, FOCUS_HANDLE_ITEM_ID_LEN, "%s", t->items[i].item_id);
         return true;
      }
   }
   return false;
}

static int cmp_id(const void *key, const void *elem) {
   return strcmp((const char *)key, *(const char *const *)elem);
}

int focus_handles_withdrawn_locked(const session_t *session,
                                   int64_t conv_id,
                                   const char *const *item_ids,
                                   int n_ids,
                                   int *out,
                                   int cap) {
   const focus_handles_t *t = session ? session->focus_handles : NULL;
   if (!t || t->conv_id != conv_id || !item_ids || n_ids <= 0) {
      return 0;
   }
   int n = 0;
   for (int i = 0; n < cap && i < t->count; i++) {
      if (bsearch(t->items[i].item_id, item_ids, (size_t)n_ids, sizeof(*item_ids), cmp_id)) {
         out[n++] = t->items[i].handle;
      }
   }
   return n;
}

void focus_handles_reset_locked(session_t *session) {
   if (session && session->focus_handles) {
      session->focus_handles->count = 0;
      session->focus_handles->conv_id = 0;
      session->focus_handles->loaded = true;
   }
}

int focus_handles_save_locked(session_t *session, int64_t conv_id, int user_id) {
   focus_handles_t *t = session ? session->focus_handles : NULL;
   if (!t || conv_id <= 0 || t->conv_id != 0 || t->count == 0) {
      return 0;
   }
   conv_focus_handle_t *put = calloc((size_t)t->count, sizeof(*put));
   if (!put) {
      return 1;
   }
   for (int i = 0; i < t->count; i++) {
      put[i] = (conv_focus_handle_t){ .source = t->items[i].source,
                                      .item_id = t->items[i].item_id,
                                      .handle = t->items[i].handle };
   }
   const int rc = conv_db_focus_handles_put(conv_id, user_id > 0 ? user_id : t->user_id, put,
                                            t->count);
   free(put);
   if (rc != AUTH_DB_SUCCESS) {
      OLOG_WARNING("focus_handles: session %u: handles for conv %lld not saved",
                   session->session_id, (long long)conv_id);
      return 1;
   }
   return 0;
}

void focus_handles_free(focus_handles_t *handles) {
   if (handles) {
      free(handles->items);
      free(handles);
   }
}
