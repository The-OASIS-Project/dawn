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
 * Programmable stubs for test_external_focus_adapters.  Stubs honor the
 * public document_db / calendar_db / memory_embeddings signatures so
 * the adapters compile and link against this test TU; the actual SQL
 * paths are tested in test_document_db / test_calendar_db (which DO
 * link the real auth_db chain).
 *
 * Network-invariant note: this stub file deliberately does NOT provide
 * stubs for any service-layer symbols (calendar_service_*,
 * email_service_*, document_search).  If the production adapter ever
 * regresses to call those, the link breaks.  This is the load-bearing
 * enforcement of the no-live-network-calls invariant; the runtime
 * counters in `ext_mock_state_t` are belt-and-suspenders for
 * ranking-changes that might re-route through unexpected stubs.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/memory_filter.h"
#include "dawn_error.h"
#include "memory/memory_embeddings.h"
#include "test_external_focus_adapters_mocks.h"
#include "tools/calendar_db.h"
#include "tools/document_db.h"

/* Touched by focus_source.c (lookup_source_weight, ranker weights);
 * tests pre-populate before each compose. */
dawn_config_t g_config;

ext_mock_state_t s_ext_mock;

void ext_mock_reset(void) {
   /* Free fixture-owned chunk-embedding allocations from any
    * previous test before zeroing.  Chunks themselves are stack/static
    * memory inside the test functions, but `chunk_embeddings[i]`
    * pointers come from the fixture's static const arrays — no free. */
   memset(&s_ext_mock, 0, sizeof(s_ext_mock));
}

/* =============================================================================
 * document_db stubs
 * ============================================================================= */

/* A new generation every call unless a test pins it: tests reconfigure the mock
 * chunk set between cases, so the real document_embed_cache must not reuse a
 * copy across them. */
int document_db_chunk_generation(int user_id, document_chunk_gen_t *gen_out) {
   static int64_t gen;
   (void)user_id;
   if (!gen_out)
      return FAILURE;
   gen_out->own = s_ext_mock.pin_generation ? s_ext_mock.generation : ++gen;
   gen_out->shared = 0;
   return SUCCESS;
}

int document_db_chunk_count(int user_id, int *count_out) {
   if (!count_out)
      return FAILURE;
   int n = 0;
   for (int i = 0; i < s_ext_mock.chunk_count; i++) {
      if (s_ext_mock.chunk_user_id[i] == user_id)
         n++;
   }
   *count_out = n;
   return SUCCESS;
}

/* Counts scans (a page starting from the beginning), not pages.  The mock is
 * one document: the cursor's chunk id is the position. */
int document_db_chunk_embeddings_page(int user_id,
                                      int dims,
                                      document_chunk_cursor_t *cursor,
                                      int max,
                                      int64_t *ids_out,
                                      float *norms_out,
                                      float *embs_out,
                                      int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!cursor || !ids_out || !norms_out || !embs_out || !count_out || dims <= 0)
      return FAILURE;
   if (cursor->done)
      return SUCCESS;
   if (cursor->chunk_id == 0)
      s_ext_mock.call_count_chunk_search_load++;
   if (s_ext_mock.fail_chunk_search)
      return FAILURE;
   /* Production skips chunks whose stored embedding size differs. */
   const bool dims_ok = s_ext_mock.chunk_dim == 0 || s_ext_mock.chunk_dim == dims;
   int n = 0;
   int read = 0;
   for (int i = 0; i < s_ext_mock.chunk_count && read < max; i++) {
      const int64_t id = s_ext_mock.chunks[i].id;
      if (s_ext_mock.chunk_user_id[i] != user_id || id <= cursor->chunk_id)
         continue;
      read++;
      cursor->chunk_id = id;
      if (!dims_ok || s_ext_mock.chunk_embeddings[i] == NULL)
         continue;
      ids_out[n] = id;
      norms_out[n] = s_ext_mock.chunks[i].embedding_norm;
      memcpy(embs_out + (size_t)n * (size_t)dims, s_ext_mock.chunk_embeddings[i],
             (size_t)dims * sizeof(float));
      n++;
   }
   if (read < max)
      cursor->done = true;
   *count_out = n;
   return SUCCESS;
}

int document_db_chunks_get_by_ids(int user_id,
                                  const int64_t *ids,
                                  int n,
                                  document_chunk_t *out,
                                  int *count_out) {
   if (ids == NULL || out == NULL || count_out == NULL || n <= 0)
      return FAILURE;
   int count = 0;
   for (int i = 0; i < s_ext_mock.chunk_count && count < n; i++) {
      if (s_ext_mock.chunk_user_id[i] != user_id)
         continue;
      for (int k = 0; k < n; k++) {
         if (ids[k] == s_ext_mock.chunks[i].id) {
            out[count] = s_ext_mock.chunks[i];
            out[count].embedding = NULL;
            count++;
            break;
         }
      }
   }
   *count_out = count;
   return SUCCESS;
}

int document_db_chunks_meta_by_ids(int user_id,
                                   const int64_t *ids,
                                   int n,
                                   doc_chunk_meta_t *out,
                                   int *count_out) {
   if (ids == NULL || out == NULL || count_out == NULL || n <= 0)
      return FAILURE;
   int count = 0;
   for (int i = 0; i < s_ext_mock.chunk_count && count < n; i++) {
      if (s_ext_mock.chunk_user_id[i] != user_id)
         continue;
      for (int k = 0; k < n; k++) {
         if (ids[k] == s_ext_mock.chunks[i].id) {
            const document_chunk_t *c = &s_ext_mock.chunks[i];
            doc_chunk_meta_t *m = &out[count++];
            memset(m, 0, sizeof(*m));
            m->id = c->id;
            m->chunk_index = c->chunk_index;
            m->document_id = c->document_id;
            m->created_at = c->created_at;
            snprintf(m->filename, sizeof(m->filename), "%s", c->doc_filename);
            break;
         }
      }
   }
   *count_out = count;
   return SUCCESS;
}

int document_db_chunk_search_bm25(int user_id,
                                  const char *query,
                                  float label_weight,
                                  float body_weight,
                                  doc_bm25_hit_t *out,
                                  float *out_scores,
                                  int max_hits,
                                  int *count_out) {
   (void)query;
   (void)label_weight;
   (void)body_weight;
   s_ext_mock.call_count_bm25++;
   int count = 0;
   for (int i = 0; i < s_ext_mock.chunk_count && count < max_hits; i++) {
      if (s_ext_mock.chunk_user_id[i] != user_id || s_ext_mock.chunk_bm25[i] <= 0.0f)
         continue;
      const document_chunk_t *c = &s_ext_mock.chunks[i];
      doc_bm25_hit_t *h = &out[count];
      memset(h, 0, sizeof(*h));
      h->id = c->id;
      h->chunk_index = c->chunk_index;
      h->document_id = c->document_id;
      h->created_at = c->created_at;
      snprintf(h->filename, sizeof(h->filename), "%s", c->doc_filename);
      out_scores[count] = s_ext_mock.chunk_bm25[i];
      count++;
   }
   *count_out = count;
   return SUCCESS;
}

/* =============================================================================
 * calendar_db stubs
 * ============================================================================= */

int calendar_db_account_list(int user_id, calendar_account_t *out, int max_count, int *count_out) {
   s_ext_mock.call_count_account_list++;
   if (s_ext_mock.fail_account_list) {
      if (count_out)
         *count_out = 0;
      return FAILURE;
   }
   if (out == NULL || count_out == NULL)
      return FAILURE;

   int n = 0;
   for (int i = 0; i < s_ext_mock.account_count && n < max_count; i++) {
      if (s_ext_mock.accounts[i].user_id != user_id)
         continue;
      out[n++] = s_ext_mock.accounts[i];
   }
   *count_out = n;
   return SUCCESS;
}

int calendar_db_calendar_list(int64_t account_id,
                              calendar_calendar_t *out,
                              int max_count,
                              int *count_out) {
   s_ext_mock.call_count_calendar_list++;
   if (out == NULL || count_out == NULL)
      return FAILURE;

   int n = 0;
   for (int i = 0; i < s_ext_mock.calendar_count && n < max_count; i++) {
      if (s_ext_mock.calendars[i].account_id != account_id)
         continue;
      out[n++] = s_ext_mock.calendars[i];
   }
   *count_out = n;
   return SUCCESS;
}

static bool occ_in_range(const calendar_occurrence_t *occ, time_t range_start, time_t range_end) {
   /* Production SQL filters cancelled. */
   if (occ->is_cancelled)
      return false;
   return occ->dtstart >= range_start && occ->dtstart <= range_end;
}

static bool calendar_id_in_set(int64_t needle, const int64_t *set, int set_count) {
   for (int i = 0; i < set_count; i++)
      if (set[i] == needle)
         return true;
   return false;
}

int calendar_db_occurrences_in_range(const int64_t *calendar_ids,
                                     int calendar_count,
                                     time_t range_start,
                                     time_t range_end,
                                     calendar_occurrence_t *out,
                                     int max_count,
                                     int *count_out) {
   s_ext_mock.call_count_occurrences_in_range++;
   if (s_ext_mock.fail_occurrences_in_range) {
      if (count_out)
         *count_out = 0;
      return FAILURE;
   }
   if (out == NULL || count_out == NULL)
      return FAILURE;
   if (calendar_count <= 0) {
      *count_out = 0;
      return SUCCESS;
   }

   int n = 0;
   for (int i = 0; i < s_ext_mock.occurrence_count && n < max_count; i++) {
      if (!calendar_id_in_set(s_ext_mock.occurrence_calendar_id[i], calendar_ids, calendar_count))
         continue;
      if (!occ_in_range(&s_ext_mock.occurrences[i], range_start, range_end))
         continue;
      out[n++] = s_ext_mock.occurrences[i];
   }
   *count_out = n;
   return SUCCESS;
}

int calendar_db_occurrences_search(const int64_t *calendar_ids,
                                   int calendar_count,
                                   const char *query,
                                   calendar_occurrence_t *out,
                                   int max_count,
                                   int *count_out) {
   s_ext_mock.call_count_occurrences_search++;
   if (s_ext_mock.fail_occurrences_search) {
      if (count_out)
         *count_out = 0;
      return FAILURE;
   }
   if (out == NULL || count_out == NULL || query == NULL)
      return FAILURE;
   if (calendar_count <= 0) {
      *count_out = 0;
      return SUCCESS;
   }

   int n = 0;
   for (int i = 0; i < s_ext_mock.occurrence_count && n < max_count; i++) {
      if (!calendar_id_in_set(s_ext_mock.occurrence_calendar_id[i], calendar_ids, calendar_count))
         continue;
      if (s_ext_mock.occurrences[i].is_cancelled)
         continue;
      /* Crude LIKE-substring match on summary or location. */
      if (strstr(s_ext_mock.occurrences[i].summary, query) == NULL &&
          strstr(s_ext_mock.occurrences[i].location, query) == NULL)
         continue;
      out[n++] = s_ext_mock.occurrences[i];
   }
   *count_out = n;
   return SUCCESS;
}

/* The remaining calendar_db_* symbols are unused by the adapter but
 * the linker may pull them when it picks the calendar_db.h header
 * declarations.  They are NOT called from any code under test —
 * abort() if they ever fire. */
int calendar_db_account_create(const calendar_account_t *acct, int64_t *id_out) {
   (void)acct;
   (void)id_out;
   abort();
}
int calendar_db_account_get(int64_t id, calendar_account_t *out) {
   (void)id;
   (void)out;
   abort();
}
int calendar_db_account_update(const calendar_account_t *acct) {
   (void)acct;
   abort();
}
int calendar_db_account_delete(int64_t id) {
   (void)id;
   abort();
}
int calendar_db_account_update_sync(int64_t id, time_t last_sync) {
   (void)id;
   (void)last_sync;
   abort();
}
int calendar_db_account_update_discovery(int64_t id, const char *p, const char *h) {
   (void)id;
   (void)p;
   (void)h;
   abort();
}
int calendar_db_account_set_read_only(int64_t id, bool ro) {
   (void)id;
   (void)ro;
   abort();
}
int calendar_db_account_set_enabled(int64_t id, bool en) {
   (void)id;
   (void)en;
   abort();
}
int calendar_db_account_list_enabled(calendar_account_t *out, int m, int *c) {
   (void)out;
   (void)m;
   (void)c;
   abort();
}
int calendar_db_calendar_create(const calendar_calendar_t *c, int64_t *o) {
   (void)c;
   (void)o;
   abort();
}
int calendar_db_calendar_get(int64_t id, calendar_calendar_t *o) {
   (void)id;
   (void)o;
   abort();
}
int calendar_db_calendar_update_ctag(int64_t id, const char *t) {
   (void)id;
   (void)t;
   abort();
}
int calendar_db_calendar_set_active(int64_t id, bool a) {
   (void)id;
   (void)a;
   abort();
}
int calendar_db_calendar_delete(int64_t id) {
   (void)id;
   abort();
}
int calendar_db_active_calendars_for_user(int u, calendar_calendar_t *o, int m, int *c) {
   (void)u;
   (void)o;
   (void)m;
   (void)c;
   abort();
}
int calendar_db_event_upsert(const calendar_event_t *e, int64_t *o) {
   (void)e;
   (void)o;
   abort();
}
int calendar_db_event_get_by_uid(const char *u, calendar_event_t *o) {
   (void)u;
   (void)o;
   abort();
}
int calendar_db_event_delete(int64_t id) {
   (void)id;
   abort();
}
int calendar_db_event_delete_by_calendar(int64_t id) {
   (void)id;
   abort();
}
int calendar_db_occurrence_insert(const calendar_occurrence_t *o, int64_t *i) {
   (void)o;
   (void)i;
   abort();
}
int calendar_db_occurrence_delete_for_event(int64_t id) {
   (void)id;
   abort();
}
int calendar_db_allday_occurrences_in_range(const int64_t *ids,
                                            int n,
                                            const char *s,
                                            const char *e,
                                            calendar_occurrence_t *o,
                                            int m,
                                            int *c) {
   (void)ids;
   (void)n;
   (void)s;
   (void)e;
   (void)o;
   (void)m;
   (void)c;
   abort();
}
int calendar_db_next_occurrence(const int64_t *ids, int n, time_t a, calendar_occurrence_t *o) {
   (void)ids;
   (void)n;
   (void)a;
   (void)o;
   abort();
}

/* document_db helpers not invoked by the adapter — same abort() guard. */
int document_db_create(int u,
                       const char *fn,
                       const char *fp,
                       const char *ft,
                       const char *fh,
                       int n,
                       bool ig,
                       int64_t *o) {
   (void)u;
   (void)fn;
   (void)fp;
   (void)ft;
   (void)fh;
   (void)n;
   (void)ig;
   (void)o;
   abort();
}
int document_db_get(int64_t id, document_t *o) {
   (void)id;
   (void)o;
   abort();
}
int document_db_find_by_hash(const char *fh, int u, int64_t *o) {
   (void)fh;
   (void)u;
   (void)o;
   abort();
}
int document_db_list(int u, document_t *o, int l, int off, int *c) {
   (void)u;
   (void)o;
   (void)l;
   (void)off;
   (void)c;
   abort();
}
int document_db_list_all(document_t *o, int l, int off, int *c) {
   (void)o;
   (void)l;
   (void)off;
   (void)c;
   abort();
}
int document_db_update_global(int64_t id, bool g) {
   (void)id;
   (void)g;
   abort();
}
int document_db_delete(int64_t id) {
   (void)id;
   abort();
}
int document_db_count_user(int u, int *c) {
   (void)u;
   (void)c;
   abort();
}
int document_db_chunk_create(int64_t d,
                             int i,
                             const char *t,
                             const float *e,
                             int dims,
                             float n,
                             int64_t ca,
                             int64_t *o) {
   (void)d;
   (void)i;
   (void)t;
   (void)e;
   (void)dims;
   (void)n;
   (void)ca;
   (void)o;
   abort();
}
int document_db_find_by_name(int u, const char *n, document_t *o) {
   (void)u;
   (void)n;
   (void)o;
   abort();
}
int document_db_chunk_read(int64_t d, document_chunk_t *c, int m, int s, int *cnt) {
   (void)d;
   (void)c;
   (void)m;
   (void)s;
   (void)cnt;
   abort();
}

/* =============================================================================
 * memory_embeddings_* stubs (cosine + availability only — no real engine)
 * ============================================================================= */

bool memory_embeddings_available(void) {
   return s_ext_mock.embeddings_available;
}

int memory_embeddings_dims(void) {
   return s_ext_mock.embeddings_available ? s_ext_mock.chunk_dim : 0;
}

float memory_embeddings_l2_norm(const float *vec, int dims) {
   double sum = 0.0;
   for (int i = 0; i < dims; i++)
      sum += (double)vec[i] * (double)vec[i];
   return (float)sqrt(sum);
}

float memory_embeddings_cosine_with_norms(const float *a,
                                          const float *b,
                                          int dims,
                                          float norm_a,
                                          float norm_b) {
   if (norm_a <= 0.0f || norm_b <= 0.0f)
      return 0.0f;
   double dot = 0.0;
   for (int i = 0; i < dims; i++)
      dot += (double)a[i] * (double)b[i];
   const double cosine = dot / ((double)norm_a * (double)norm_b);
   if (cosine < 0.0)
      return 0.0f;
   if (cosine > 1.0)
      return 1.0f;
   return (float)cosine;
}

/* embedding_engine_* (the real document_embed_cache links against these). */
float embedding_engine_l2_norm(const float *vec, int dims) {
   return memory_embeddings_l2_norm(vec, dims);
}

float embedding_engine_cosine_with_norms(const float *a,
                                         const float *b,
                                         int dims,
                                         float norm_a,
                                         float norm_b) {
   return memory_embeddings_cosine_with_norms(a, b, dims, norm_a, norm_b);
}

float embedding_corpus_relevance(float cosine, double cosine_sum, int pool) {
   if (pool <= 0)
      return 0.0f;
   const double mean = cosine_sum / (double)pool;
   if (mean >= 1.0)
      return 0.0f;
   return (float)(((double)cosine - mean) / (1.0 - mean));
}

/* memory_filter — production blocklist NOT linked.  Tests never inject
 * blocklist payloads through the adapter path; framework filter-on-
 * retrieval is exercised in test_focus_source.c (which links the real
 * filter).  Stub returns false so candidates pass through. */
bool memory_filter_check(const char *text) {
   (void)text;
   return false;
}
