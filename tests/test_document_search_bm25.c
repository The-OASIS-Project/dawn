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
 * v61 document lexical (BM25) search — the "bios" search-quality test.
 *
 * Reproduces the core of conversation 809: three near-twin reference notes
 * stored under distinct labels.  Asserts the column-weighted BM25 lexical
 * channel (document_db_chunk_search_bm25) returns the EXACT-label match rank-1,
 * so a label query is not buried under its semantic neighbors.  The lexical
 * channel uses no embeddings, so this needs only the real auth_db + FTS index.
 */

#include <stdlib.h> /* free */
#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "memory/memory_stem.h"
#include "memory/memory_types.h" /* MEMORY_FACT_STEMS_MAX */
#include "tools/document_db.h"
#include "unity.h"

/* document_db.c reads g_config (v62 versioning); zero-init = versioning off, so
 * these search/edit tests are unaffected by the archive path. */
dawn_config_t g_config;

/* Match the production BM25 column-weight defaults (config_defaults.c). */
#define TEST_BM25_LABEL_WEIGHT 3.0f
#define TEST_BM25_BODY_WEIGHT 1.0f

/* Seed one single-chunk "note": a document whose filename IS the label, plus its
 * one chunk indexed into document_chunks_fts (label + body stemmed). */
static int64_t seed_note(int user_id, const char *label, const char *body, int idx) {
   char hash[65];
   snprintf(hash, sizeof(hash), "hash_%d", idx);
   int64_t doc_id = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_create(user_id, label, label, "note", hash, 1, false,
                                                     &doc_id));

   float emb[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
   int64_t chunk_id = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         document_db_chunk_create(doc_id, 0, body, emb, 4, 1.0f, 0, &chunk_id));

   char label_stems[MEMORY_FACT_STEMS_MAX];
   char body_stems[MEMORY_FACT_STEMS_MAX];
   memory_stem_string(label, label_stems, sizeof(label_stems));
   memory_stem_string(body, body_stems, sizeof(body_stems));
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_chunk_index_fts(chunk_id, label_stems, body_stems));
   return doc_id;
}

void setUp(void) {
   auth_db_init(":memory:");
   memory_stem_init();
   auth_db_create_user("tester", "hash", true); /* user id 1 */

   seed_note(1, "Public Bio",
             "Jon Smith is a passionate maker and cosplayer who builds the things that inspire "
             "him from science fiction.",
             1);
   seed_note(1, "Dragon Con Bio",
             "Jon Smith is a passionate maker and cosplayer who shares his knowledge on 3D "
             "printing through his YouTube channels.",
             2);
   seed_note(1, "Open Sauce About You",
             "Jon Smith is an embedded systems engineer developing open-source home and wearable "
             "technology.",
             3);
}

void tearDown(void) {
   auth_db_shutdown();
}

/* The exact-label query "public bio" must return the Public Bio note rank-1 —
 * its label matches BOTH query terms, while the near-twins match only "bio" on
 * the (heavily weighted) label column. */
void test_exact_label_ranks_first(void) {
   doc_bm25_hit_t hits[10];
   float scores[10];
   int count = 0;
   TEST_ASSERT_EQUAL_INT(
       SUCCESS, document_db_chunk_search_bm25(1, "public bio", TEST_BM25_LABEL_WEIGHT,
                                              TEST_BM25_BODY_WEIGHT, hits, scores, 10, &count));
   TEST_ASSERT_GREATER_THAN_INT(0, count);
   TEST_ASSERT_EQUAL_STRING("Public Bio", hits[0].filename);
   TEST_ASSERT_TRUE(scores[0] > 0.0f);
}

/* A label query for a different note returns that note rank-1 — confirms the
 * label channel discriminates between the near-twins rather than collapsing. */
void test_other_label_ranks_first(void) {
   doc_bm25_hit_t hits[10];
   float scores[10];
   int count = 0;
   TEST_ASSERT_EQUAL_INT(
       SUCCESS, document_db_chunk_search_bm25(1, "dragon con bio", TEST_BM25_LABEL_WEIGHT,
                                              TEST_BM25_BODY_WEIGHT, hits, scores, 10, &count));
   TEST_ASSERT_GREATER_THAN_INT(0, count);
   TEST_ASSERT_EQUAL_STRING("Dragon Con Bio", hits[0].filename);
}

/* A body-only term ("cosplayer") still retrieves via the body column even though
 * it is absent from every label. */
void test_body_term_retrieves(void) {
   doc_bm25_hit_t hits[10];
   float scores[10];
   int count = 0;
   TEST_ASSERT_EQUAL_INT(
       SUCCESS, document_db_chunk_search_bm25(1, "cosplayer", TEST_BM25_LABEL_WEIGHT,
                                              TEST_BM25_BODY_WEIGHT, hits, scores, 10, &count));
   TEST_ASSERT_GREATER_THAN_INT(0, count);
}

/* Resolve a note's document_id by an exact-label query (rank-1). */
static int64_t doc_id_for_label(const char *label_query) {
   doc_bm25_hit_t hits[10];
   float scores[10];
   int count = 0;
   document_db_chunk_search_bm25(1, label_query, TEST_BM25_LABEL_WEIGHT, TEST_BM25_BODY_WEIGHT,
                                 hits, scores, 10, &count);
   return count > 0 ? hits[0].document_id : 0;
}

/* M-5: a stable-id note edit keeps doc_id, swaps the FTS rows (new label
 * searchable, old label gone), and stays single-chunk. */
void test_note_update_stable_id(void) {
   int64_t doc_id = doc_id_for_label("public bio");
   TEST_ASSERT_GREATER_THAN_INT(0, doc_id);

   float emb[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_note_update(1, doc_id, "Public Profile",
                                                          "Jon Smith is an engineer and maker.",
                                                          emb, 4, 1.0f, "hash_updated"));

   /* doc_id stable + new label retrievable rank-1. */
   doc_bm25_hit_t hits[10];
   float scores[10];
   int count = 0;
   document_db_chunk_search_bm25(1, "public profile", TEST_BM25_LABEL_WEIGHT, TEST_BM25_BODY_WEIGHT,
                                 hits, scores, 10, &count);
   TEST_ASSERT_GREATER_THAN_INT(0, count);
   TEST_ASSERT_EQUAL_STRING("Public Profile", hits[0].filename);
   TEST_ASSERT_EQUAL_INT64(doc_id, hits[0].document_id);

   /* num_chunks still 1. */
   document_t doc;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_get(doc_id, &doc));
   TEST_ASSERT_EQUAL_INT(1, doc.num_chunks);

   /* Old label "Public Bio" no longer exists as a label. */
   count = 0;
   document_db_chunk_search_bm25(1, "public bio", TEST_BM25_LABEL_WEIGHT, TEST_BM25_BODY_WEIGHT,
                                 hits, scores, 10, &count);
   for (int i = 0; i < count; i++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(hits[i].filename, "Public Bio"));
}

/* delete_indexed removes the doc AND its FTS rows (no orphans): the deleted
 * label stops matching, siblings remain. */
void test_note_delete_indexed(void) {
   int64_t doc_id = doc_id_for_label("dragon con bio");
   TEST_ASSERT_GREATER_THAN_INT(0, doc_id);
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_delete_indexed(doc_id));

   doc_bm25_hit_t hits[10];
   float scores[10];
   int count = 0;
   document_db_chunk_search_bm25(1, "dragon con bio", TEST_BM25_LABEL_WEIGHT, TEST_BM25_BODY_WEIGHT,
                                 hits, scores, 10, &count);
   for (int i = 0; i < count; i++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(hits[i].filename, "Dragon Con Bio"));

   /* A sibling note is untouched. */
   TEST_ASSERT_GREATER_THAN_INT(0, doc_id_for_label("public bio"));
}

/* M-5: exact-label routing must NOT match a substring — "Bio" must not resolve
 * to "Public Bio" (else a save/delete would clobber the wrong note). */
void test_find_by_label_exact(void) {
   document_t doc;
   /* Exact full label hits. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_find_by_label_exact(1, "Public Bio", true, &doc));
   TEST_ASSERT_EQUAL_STRING("Public Bio", doc.filename);
   /* Case-insensitive. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_find_by_label_exact(1, "public bio", true, &doc));
   /* Substring must NOT match. */
   TEST_ASSERT_EQUAL_INT(FAILURE, document_db_find_by_label_exact(1, "Bio", true, &doc));
   TEST_ASSERT_EQUAL_INT(FAILURE, document_db_find_by_label_exact(1, "Public", true, &doc));
}

/* v61 recovery: the admin rebuild clears + re-indexes every live chunk from
 * scratch, so searches keep working after a from-scratch rebuild. */
void test_rebuild_fts_reindexes_all(void) {
   int count = -1;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_rebuild_fts(&count));
   TEST_ASSERT_EQUAL_INT(3, count); /* three seeded single-chunk notes */

   doc_bm25_hit_t hits[10];
   float scores[10];
   int n = 0;
   document_db_chunk_search_bm25(1, "public bio", TEST_BM25_LABEL_WEIGHT, TEST_BM25_BODY_WEIGHT,
                                 hits, scores, 10, &n);
   TEST_ASSERT_GREATER_THAN_INT(0, n);
   TEST_ASSERT_EQUAL_STRING("Public Bio", hits[0].filename);
}

/* Rebuild scopes the index to LIVE chunks only.  A plain document_db_delete
 * leaves an FTS orphan (the recovery scenario); the from-scratch rebuild reindexes
 * 2, not 3, and the survivors still retrieve. */
void test_rebuild_fts_scopes_to_live_chunks(void) {
   int64_t doc_id = doc_id_for_label("dragon con bio");
   TEST_ASSERT_GREATER_THAN_INT(0, doc_id);
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_delete(doc_id)); /* plain delete → FTS orphan */

   int count = -1;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_rebuild_fts(&count));
   TEST_ASSERT_EQUAL_INT(2, count);

   TEST_ASSERT_GREATER_THAN_INT(0, doc_id_for_label("public bio"));
}

/* =============================================================================
 * v62 document versioning (soft-archive / undo / restore)
 * ============================================================================= */

/* An in-place note edit archives the pre-edit content as a restorable version. */
void test_version_archive_on_edit(void) {
   g_config.documents.version_retention_days = 14; /* enable versioning */
   int64_t doc = seed_note(1, "Versioned", "ORIGINAL body text here", 10);
   float emb[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_note_update(1, doc, "Versioned", "REVISED body text",
                                                          emb, 4, 1.0f, "vh2"));
   document_version_meta_t v[8];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_version_list(1, doc, v, 8, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_NOT_NULL(strstr(v[0].preview, "ORIGINAL"));
}

/* Deleting a note archives its content first — the version SURVIVES the delete,
 * and its full text is fetchable for restore (owner-scoped). */
void test_version_survives_delete(void) {
   g_config.documents.version_retention_days = 14;
   int64_t doc = seed_note(1, "ToDelete", "content worth keeping", 11);
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_delete_indexed(doc));
   document_version_meta_t v[8];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_version_list(1, doc, v, 8, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   char *full = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_version_get_text(v[0].id, 1, &full, NULL, 0, NULL));
   TEST_ASSERT_EQUAL_STRING("content worth keeping", full);
   free(full);
}

/* Versions are owner-scoped: another user can neither list nor fetch them. */
void test_version_owner_scoped(void) {
   g_config.documents.version_retention_days = 14;
   int64_t doc = seed_note(1, "PrivateV", "secret note body", 12);
   float emb[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
   document_db_note_update(1, doc, "PrivateV", "edited", emb, 4, 1.0f, "vh3");
   document_version_meta_t v[8];
   int n = 0;
   document_db_version_list(1, doc, v, 8, &n);
   TEST_ASSERT_EQUAL_INT(1, n);
   int n2 = -1;
   document_db_version_list(2, doc, v, 8, &n2); /* user 2 */
   TEST_ASSERT_EQUAL_INT(0, n2);
   char *full = NULL;
   TEST_ASSERT_EQUAL_INT(FAILURE, document_db_version_get_text(v[0].id, 2, &full, NULL, 0, NULL));
   TEST_ASSERT_NULL(full);
}

/* The per-document cap retains only the N newest versions. */
void test_version_per_doc_cap(void) {
   g_config.documents.version_retention_days = 14;
   g_config.documents.version_keep_per_doc = 10;
   int64_t doc = seed_note(1, "Churn", "v0", 13);
   float emb[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
   char body[16], hash[16];
   for (int i = 1; i <= 14; i++) { /* 14 edits → capped to version_keep_per_doc (10) */
      snprintf(body, sizeof(body), "v%d", i);
      snprintf(hash, sizeof(hash), "vc%d", i);
      document_db_note_update(1, doc, "Churn", body, emb, 4, 1.0f, hash);
   }
   document_version_meta_t v[DOC_VERSION_MAX_LIST];
   int n = 0;
   document_db_version_list(1, doc, v, DOC_VERSION_MAX_LIST, &n);
   TEST_ASSERT_EQUAL_INT(10, n);
}

/* Versioning disabled (retention 0) archives nothing. */
void test_version_disabled_archives_nothing(void) {
   g_config.documents.version_retention_days = 0; /* off */
   int64_t doc = seed_note(1, "NoVer", "body", 14);
   float emb[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
   document_db_note_update(1, doc, "NoVer", "edited", emb, 4, 1.0f, "vh9");
   document_version_meta_t v[8];
   int n = -1;
   document_db_version_list(1, doc, v, 8, &n);
   TEST_ASSERT_EQUAL_INT(0, n);
}

/* =============================================================================
 * v63 multi-chunk document full-text storage + in-place edit (B1b)
 * ============================================================================= */

/* Full text round-trips and is owner-scoped. */
void test_full_text_set_get(void) {
   int64_t id = 0;
   document_db_create(1, "doc.txt", "doc.txt", "txt", "fthash", 1, false, &id);
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_full_text_set(id, "the whole document body"));
   char *t = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_full_text_get(id, 1, &t));
   TEST_ASSERT_EQUAL_STRING("the whole document body", t);
   free(t);
   char *t2 = NULL;
   TEST_ASSERT_EQUAL_INT(FAILURE, document_db_full_text_get(id, 2, &t2)); /* user 2 */
   TEST_ASSERT_NULL(t2);
}

/* A document with no stored full text (pre-v63 upload) is not editable in place. */
void test_doc_read_for_edit_requires_full_text(void) {
   int64_t id = 0;
   document_db_create(1, "legacy.txt", "legacy.txt", "txt", "lhash", 1, false, &id);
   char fn[DOC_FILENAME_MAX];
   char *full = NULL;
   int64_t *ids = NULL;
   char **texts = NULL;
   int n = 0;
   TEST_ASSERT_EQUAL_INT(FAILURE, document_db_doc_read_for_edit(1, id, fn, sizeof(fn), &full, &ids,
                                                                &texts, &n));
   TEST_ASSERT_NULL(full);
}

/* An in-place multi-chunk replace swaps content + chunks (doc_id stable), updates
 * full text + num_chunks, archives the old content, and re-indexes FTS. */
void test_doc_replace_in_place(void) {
   g_config.documents.version_retention_days = 14;
   int64_t id = 0;
   document_db_create(1, "Manual", "Manual", "txt", "mh0", 2, false, &id);
   float emb[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
   int64_t c0 = 0, c1 = 0;
   document_db_chunk_create(id, 0, "alpha chunk", emb, 4, 1.0f, 0, &c0);
   document_db_chunk_create(id, 1, "bravo chunk", emb, 4, 1.0f, 0, &c1);
   document_db_full_text_set(id, "alpha chunk bravo chunk");
   char ls[MEMORY_FACT_STEMS_MAX], bs0[MEMORY_FACT_STEMS_MAX], bs1[MEMORY_FACT_STEMS_MAX];
   memory_stem_string("Manual", ls, sizeof(ls));
   memory_stem_string("alpha chunk", bs0, sizeof(bs0));
   memory_stem_string("bravo chunk", bs1, sizeof(bs1));
   document_db_chunk_index_fts(c0, ls, bs0);
   document_db_chunk_index_fts(c1, ls, bs1);

   char ns[MEMORY_FACT_STEMS_MAX];
   memory_stem_string("charlie content", ns, sizeof(ns));
   doc_replace_chunk_t nc[1] = { { "charlie content", emb, 1.0f, ns } };
   int64_t old_ids[2] = { c0, c1 };
   const char *old_bs[2] = { bs0, bs1 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_doc_replace(1, id, "charlie content", "mh1", "Manual",
                                                          ls, old_ids, old_bs, 2, nc, 1, 4));

   document_t d;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_get(id, &d));
   TEST_ASSERT_EQUAL_INT(1, d.num_chunks); /* 2 → 1, same doc id */

   char *ft = NULL;
   document_db_full_text_get(id, 1, &ft);
   TEST_ASSERT_EQUAL_STRING("charlie content", ft);
   free(ft);

   doc_bm25_hit_t hits[10];
   float sc[10];
   int hn = 0;
   document_db_chunk_search_bm25(1, "charlie", TEST_BM25_LABEL_WEIGHT, TEST_BM25_BODY_WEIGHT, hits,
                                 sc, 10, &hn);
   TEST_ASSERT_GREATER_THAN_INT(0, hn); /* new content searchable */
   int hn2 = 0;
   document_db_chunk_search_bm25(1, "bravo", TEST_BM25_LABEL_WEIGHT, TEST_BM25_BODY_WEIGHT, hits,
                                 sc, 10, &hn2);
   TEST_ASSERT_EQUAL_INT(0, hn2); /* old chunk gone from FTS (no orphan) */

   document_version_meta_t v[8];
   int vn = 0;
   document_db_version_list(1, id, v, 8, &vn);
   TEST_ASSERT_EQUAL_INT(1, vn);
   TEST_ASSERT_NOT_NULL(strstr(v[0].preview, "alpha")); /* old content archived */
}

/* Recently-deleted list: a deleted item's surviving snapshot is recoverable
 * (its document is gone, so it shows up), owner-scoped. */
void test_version_list_deleted(void) {
   g_config.documents.version_retention_days = 14;
   int64_t doc = seed_note(1, "DeleteMe", "important content", 20);
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_delete_indexed(doc)); /* archives, then deletes */

   document_version_meta_t v[8];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_version_list_deleted(1, v, 8, &n));
   TEST_ASSERT_EQUAL_INT(1, n); /* only the deleted item, not the still-existing seeds */
   TEST_ASSERT_EQUAL_STRING("DeleteMe", v[0].filename);
   TEST_ASSERT_NOT_NULL(strstr(v[0].preview, "important"));

   char *full = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_version_get_text(v[0].id, 1, &full, NULL, 0, NULL));
   TEST_ASSERT_EQUAL_STRING("important content", full);
   free(full);

   int n2 = -1;
   document_db_version_list_deleted(2, v, 8, &n2); /* user 2 */
   TEST_ASSERT_EQUAL_INT(0, n2);
}

/* Undo-last-change mechanic (what 'recover' does for an existing item): restoring
 * the newest archived version's text brings the item back to its prior state and
 * archives the just-undone state (so it stays redo-able). */
void test_version_undo_round_trip(void) {
   g_config.documents.version_retention_days = 14;
   int64_t doc = seed_note(1, "Undoable", "ORIGINAL", 21);
   float emb[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
   document_db_note_update(1, doc, "Undoable", "CHANGED", emb, 4, 1.0f,
                           "uh1"); /* archives ORIGINAL */

   document_version_meta_t v[8];
   int n = 0;
   document_db_version_list(1, doc, v, 8, &n);
   TEST_ASSERT_EQUAL_INT(1, n);
   char *vtext = NULL;
   document_db_version_get_text(v[0].id, 1, &vtext, NULL, 0, NULL);
   TEST_ASSERT_EQUAL_STRING("ORIGINAL", vtext);

   /* Undo: restore the newest version in place. */
   document_db_note_update(1, doc, "Undoable", vtext, emb, 4, 1.0f, "uh2");
   free(vtext);

   document_chunk_t chunk;
   int cc = 0;
   document_db_chunk_read(doc, &chunk, 1, 0, &cc);
   TEST_ASSERT_EQUAL_STRING("ORIGINAL", chunk.text); /* back to the prior state */
   n = 0;
   document_db_version_list(1, doc, v, 8, &n);
   TEST_ASSERT_EQUAL_INT(2, n); /* ORIGINAL (pre-change) + CHANGED (pre-undo, now redo-able) */
}

/* Recover re-points a deleted item's snapshots onto the re-created doc: the
 * version leaves the "Recently deleted" list and attaches to the new live doc.
 * Owner-scoped — a wrong-user reattach is a no-op. */
void test_version_reattach_on_recover(void) {
   g_config.documents.version_retention_days = 14;
   int64_t old_doc = seed_note(1, "Reattach", "snapshot body", 22);
   /* Bump the rowid counter so the re-created doc below gets a DISTINCT id (else
    * SQLite reuses old_doc's freed rowid, in which case the snapshots already
    * point at the now-live id and no reattach is needed — the no-op case). */
   seed_note(1, "RowidFiller", "x", 99);
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_delete_indexed(old_doc)); /* archives + deletes */

   document_version_meta_t v[8];
   int n = 0;
   document_db_version_list_deleted(1, v, 8, &n);
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(old_doc, v[0].document_id);

   /* Simulate recover re-creating the item under a fresh doc id. */
   int64_t new_doc = seed_note(1, "Reattach", "snapshot body", 23);
   TEST_ASSERT_TRUE(new_doc != old_doc);

   /* Wrong-user reattach is a no-op — still listed as deleted. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_version_reattach(2, old_doc, new_doc));
   n = -1;
   document_db_version_list_deleted(1, v, 8, &n);
   TEST_ASSERT_EQUAL_INT(1, n);

   /* Correct owner re-points: no longer deleted, history hangs off the new doc. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_version_reattach(1, old_doc, new_doc));
   n = -1;
   document_db_version_list_deleted(1, v, 8, &n);
   TEST_ASSERT_EQUAL_INT(0, n);
   n = 0;
   document_db_version_list(1, new_doc, v, 8, &n);
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("Reattach", v[0].filename);
}

/* set_num_chunks corrects the stored count (e.g. after partial embed failures). */
void test_set_num_chunks(void) {
   int64_t doc = seed_note(1, "Counted", "body", 24); /* created with num_chunks = 1 */
   document_t d;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_get(doc, &d));
   TEST_ASSERT_EQUAL_INT(1, d.num_chunks);

   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_set_num_chunks(doc, 3));
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_db_get(doc, &d));
   TEST_ASSERT_EQUAL_INT(3, d.num_chunks);

   TEST_ASSERT_EQUAL_INT(FAILURE, document_db_set_num_chunks(0, 1));    /* invalid id */
   TEST_ASSERT_EQUAL_INT(FAILURE, document_db_set_num_chunks(doc, -1)); /* invalid count */
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_exact_label_ranks_first);
   RUN_TEST(test_other_label_ranks_first);
   RUN_TEST(test_body_term_retrieves);
   RUN_TEST(test_note_update_stable_id);
   RUN_TEST(test_note_delete_indexed);
   RUN_TEST(test_find_by_label_exact);
   RUN_TEST(test_rebuild_fts_reindexes_all);
   RUN_TEST(test_rebuild_fts_scopes_to_live_chunks);
   RUN_TEST(test_version_archive_on_edit);
   RUN_TEST(test_version_survives_delete);
   RUN_TEST(test_version_owner_scoped);
   RUN_TEST(test_version_per_doc_cap);
   RUN_TEST(test_version_disabled_archives_nothing);
   RUN_TEST(test_full_text_set_get);
   RUN_TEST(test_doc_read_for_edit_requires_full_text);
   RUN_TEST(test_doc_replace_in_place);
   RUN_TEST(test_version_list_deleted);
   RUN_TEST(test_version_undo_round_trip);
   RUN_TEST(test_version_reattach_on_recover);
   RUN_TEST(test_set_num_chunks);
   return UNITY_END();
}
