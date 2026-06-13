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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Document Database Layer - CRUD for documents and document_chunks tables
 *
 * Part of the RAG document search system. Uses the shared auth_db handle
 * and prepared statements. All functions are thread-safe via the auth_db mutex.
 */

#ifndef DOCUMENT_DB_H
#define DOCUMENT_DB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#include <stddef.h> /* size_t */

#define DOC_FILENAME_MAX 256
#define DOC_FILEPATH_MAX 512
#define DOC_FILETYPE_MAX 16
#define DOC_HASH_MAX 65 /* SHA-256 hex + null */
#define DOC_MAX_RESULTS 100
#define DOC_CHUNK_TEXT_MAX 4096
#define DOC_VERSION_PREVIEW_MAX 160 /* v62: text preview length in a version-list row */
#define DOC_VERSION_MAX_LIST 32     /* v62: cap on versions returned by a list call */

/* =============================================================================
 * Types
 * ============================================================================= */

typedef struct {
   int64_t id;
   int user_id;
   char filename[DOC_FILENAME_MAX];
   char filepath[DOC_FILEPATH_MAX];
   char filetype[DOC_FILETYPE_MAX];
   char file_hash[DOC_HASH_MAX];
   int num_chunks;
   bool is_global;
   int64_t created_at;
   char owner_name[65]; /* Populated only by document_db_list_all (JOIN) */
} document_t;

typedef struct {
   int64_t id;
   int chunk_index;
   char text[DOC_CHUNK_TEXT_MAX];
   float *embedding; /* Caller-managed buffer */
   float embedding_norm;
   int64_t document_id;
   char doc_filename[DOC_FILENAME_MAX];
   char doc_filetype[DOC_FILETYPE_MAX];
   int64_t created_at; /* v35 — chunk origin timestamp; 0 = unknown */
} document_chunk_t;

/* v62: one row of a document/note's version history (metadata + text preview).
 * Full text for restore is fetched separately via document_db_version_get_text. */
typedef struct {
   int64_t id;          /* document_versions.id (restore key) */
   int64_t document_id; /* the (possibly-deleted) original document id */
   char filename[DOC_FILENAME_MAX];
   char preview[DOC_VERSION_PREVIEW_MAX];
   int64_t archived_at;
} document_version_meta_t;

/* v61: one lexical (BM25) candidate from document_db_chunk_search_bm25.  Carries
 * enough to fuse with the semantic channel and format a citation — no embedding
 * (the lexical side doesn't need it). */
typedef struct {
   int64_t id; /* chunk id (== FTS rowid) */
   int chunk_index;
   int64_t document_id;
   char filename[DOC_FILENAME_MAX]; /* the label, for label-match scoring */
   char filetype[DOC_FILETYPE_MAX];
   int num_chunks; /* parent doc chunk count (1 == note / whole-record) */
   int64_t created_at;
   char text[DOC_CHUNK_TEXT_MAX];
} doc_bm25_hit_t;

/* One literal-grep hit: just the coordinates needed to fetch a context window
 * (the matching text comes from document_db_chunk_read_range, not duplicated
 * here — keeps the hit array small when paging through many matches). */
typedef struct {
   int chunk_index;
   int64_t document_id;
   char filename[DOC_FILENAME_MAX]; /* for the citation */
   int num_chunks;                  /* parent doc chunk count — clamps the context window */
} doc_grep_hit_t;

/* =============================================================================
 * Document CRUD
 * ============================================================================= */

/**
 * @brief Create a new document record
 *
 * @param user_id  Owner (0 for global/filesystem-ingested)
 * @param filename Display name
 * @param filepath Original path
 * @param filetype Extension (pdf, docx, txt, md)
 * @param file_hash SHA-256 hex string
 * @param num_chunks Number of chunks
 * @param is_global Whether accessible to all users
 * @param[out] id_out Created document ID (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_create(int user_id,
                       const char *filename,
                       const char *filepath,
                       const char *filetype,
                       const char *file_hash,
                       int num_chunks,
                       bool is_global,
                       int64_t *id_out);

/**
 * @brief Correct a document's stored chunk count (e.g. after embed failures).
 * @param doc_id     Document id to update.
 * @param num_chunks New chunk count (must be >= 0).
 * @return SUCCESS (0) / FAILURE (1 = invalid args / DB error).
 */
int document_db_set_num_chunks(int64_t doc_id, int num_chunks);

/**
 * @brief Get a document by ID
 * @return SUCCESS (0) on success, FAILURE (1) on not found/error
 */
int document_db_get(int64_t doc_id, document_t *out);

/**
 * @brief Check if a document with this hash already exists for the user
 *
 * @param file_hash SHA-256 hex string
 * @param user_id   User ID
 * @param[out] id_out Existing document ID if found, 0 if not found (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_find_by_hash(const char *file_hash, int user_id, int64_t *id_out);

/**
 * @brief List documents accessible to a user (own + global), paginated
 *
 * @param user_id   User ID
 * @param out       Output array (caller allocates)
 * @param limit     Maximum documents to return
 * @param offset    Pagination offset
 * @param[out] count_out Number of documents written to out[] (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_list(int user_id, document_t *out, int limit, int offset, int *count_out);

/**
 * @brief List all documents across all users (admin view), paginated
 *
 * @param out       Output array (caller allocates)
 * @param limit     Maximum documents to return
 * @param offset    Pagination offset
 * @param[out] count_out Number of documents written to out[] (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_list_all(document_t *out, int limit, int offset, int *count_out);

/**
 * @brief Update a document's global visibility flag
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_update_global(int64_t doc_id, bool is_global);

/**
 * @brief Delete a document and all its chunks (cascade)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_delete(int64_t doc_id);

/**
 * @brief Delete a document, its chunks, AND its document_chunks_fts rows (v61).
 *
 * The contentless FTS5 index is NOT reached by the document_chunks FK cascade
 * (no SQL trigger can run the C stemmer), so a plain document_db_delete leaves
 * orphan FTS rows that bias global IDF.  Use this for any document that may have
 * been FTS-indexed.  Permission checks remain the caller's responsibility
 * (mirrors document_db_delete).
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_delete_indexed(int64_t doc_id);

/**
 * @brief Index one chunk into document_chunks_fts (v61 BM25 lexical channel).
 *
 * Called by the ingest pipeline right after document_db_chunk_create.  Stems are
 * computed by the CALLER outside the auth_db lock (leaf-lock rule); this performs
 * only the locked insert.  Soft: a no-op (returns FAILURE) if the v61 migration
 * has not completed, so ingest still succeeds and search degrades to semantic.
 *
 * @param chunk_id      document_chunks.id (== FTS rowid)
 * @param label_stems   pre-stemmed filename/label (memory_stem_string output)
 * @param body_stems    pre-stemmed chunk text
 * @return SUCCESS (0) on success, FAILURE (1) if not indexed
 */
int document_db_chunk_index_fts(int64_t chunk_id, const char *label_stems, const char *body_stems);

/**
 * @brief Rebuild the entire document_chunks_fts index from scratch (v61 recovery).
 *
 * Recovery path for a partial v61 migration backfill (version advances even if the
 * backfill is interrupted) or FTS orphans left by delete_indexed's OOM fallback.
 * Clears the contentless index ('delete-all'), then re-stems + re-inserts every
 * live chunk.  Stemming runs outside the auth_db leaf lock.  Global (the FTS index
 * spans all users); admin-gated by the socket.  Safe to re-run (idempotent).
 *
 * @param count_out Optional: number of chunks indexed (may be NULL).
 * @return SUCCESS (0) on success, FAILURE (1) on error (incl. migration not run).
 */
int document_db_rebuild_fts(int *count_out);

/* =============================================================================
 * v62: document versioning (soft-archive / undo / restore)
 * ============================================================================= */

/**
 * @brief List a document's archived versions, newest first, owner-scoped.
 * @param out Caller-allocated array (>= max); fills metadata + text preview.
 * @return SUCCESS (0) / FAILURE (1).
 */
int document_db_version_list(int user_id,
                             int64_t doc_id,
                             document_version_meta_t *out,
                             int max,
                             int *count_out);

/**
 * @brief List recently-deleted items (newest surviving version per now-deleted
 * document), owner-scoped, newest first.  Each `id` restores via re-create.
 * @return SUCCESS (0) / FAILURE (1).
 */
int document_db_version_list_deleted(int user_id,
                                     document_version_meta_t *out,
                                     int max,
                                     int *count_out);

/**
 * @brief Fetch a version's FULL text for restore, owner-scoped.
 * @param text_out     Heap string on success (caller frees); NULL otherwise.
 * @param filename_out Optional out buffer for the archived filename.
 * @param doc_id_out   Optional out for the original document id.
 * @return SUCCESS (0) / FAILURE (1 = not found / not owned / OOM).
 */
int document_db_version_get_text(int64_t version_id,
                                 int user_id,
                                 char **text_out,
                                 char *filename_out,
                                 size_t fn_sz,
                                 int64_t *doc_id_out);

/**
 * @brief Re-point a deleted doc's version rows onto the doc created on recover.
 *
 * Clears the "Recently deleted" ghost (its versions no longer dangle) and
 * carries history through the delete/recover cycle.  Owner-scoped; best-effort.
 * @return SUCCESS (0) / FAILURE (1 = invalid ids / DB error).
 */
int document_db_version_reattach(int user_id, int64_t old_doc_id, int64_t new_doc_id);

/**
 * @brief Retention sweep — drop versions older than retention_days (global).
 * @param deleted_out Optional count of rows removed.
 * @return SUCCESS (0) / FAILURE (1).  No-op when retention_days <= 0.
 */
int document_db_version_prune_expired(int retention_days, int *deleted_out);

/* =============================================================================
 * v63: multi-chunk document full-text storage + in-place edit (B1b)
 * ============================================================================= */

/** Store/replace a document's canonical un-chunked full text (own lock). */
int document_db_full_text_set(int64_t doc_id, const char *text);

/**
 * @brief Fetch a document's canonical full text, owner-scoped.
 * @param text_out Heap string on success (caller frees).
 * @return SUCCESS (0) / FAILURE (1 = no stored full text → must be re-saved).
 */
int document_db_full_text_get(int64_t doc_id, int user_id, char **text_out);

/* One prepared chunk for an in-place document replace (all borrowed). */
typedef struct {
   const char *text;
   const float *embedding; /* `dims` floats */
   float norm;
   const char *body_stems; /* pre-stemmed chunk body */
} doc_replace_chunk_t;

/**
 * @brief Read a multi-chunk document for in-place editing (owner-checked, must
 * have stored full text).  Returns filename + full text + old chunk ids/texts so
 * the caller can stem outside the lock before document_db_doc_replace.
 * Caller frees *full_text_out, *old_ids_out, each (*old_texts_out)[i] + the array.
 * @return SUCCESS (0) / FAILURE (1 = not owned / no full text / OOM).
 */
int document_db_doc_read_for_edit(int user_id,
                                  int64_t doc_id,
                                  char *filename_out,
                                  size_t fn_sz,
                                  char **full_text_out,
                                  int64_t **old_ids_out,
                                  char ***old_texts_out,
                                  int *n_out);

/**
 * @brief Atomic in-place replace of a multi-chunk document's content (B1b).
 * Archives the old full text as a version, swaps all chunks + FTS rows, updates
 * num_chunks + file_hash, stores the new full text — doc_id / is_global stable.
 * Stems are computed by the caller (leaf-lock); label_stems is shared by all
 * chunks (filename unchanged).
 * @return SUCCESS (0) / FAILURE (1, rolled back).
 */
int document_db_doc_replace(int user_id,
                            int64_t doc_id,
                            const char *new_full_text,
                            const char *new_hash,
                            const char *filename,
                            const char *label_stems,
                            const int64_t *old_ids,
                            const char *const *old_body_stems,
                            int n_old,
                            const doc_replace_chunk_t *new_chunks,
                            int n_new,
                            int dims);

/**
 * @brief Stable-id in-place edit of a single-chunk note (v61).
 *
 * Gated on filetype == "note" && num_chunks == 1 && owner == user_id — a forged
 * request cannot rewrite an uploaded multi-chunk document.  Swaps the chunk text
 * + embedding, rebuilds the two FTS rows, and updates the document filename +
 * file_hash in ONE transaction, keeping doc_id / is_global / inbound pointers
 * stable.  The new embedding + new_hash are computed by the caller (the engine /
 * sha256 live a layer up); stemming is done here from the live + new text.
 *
 * @param user_id   Owner (gate)
 * @param doc_id    Note document id
 * @param new_label New label (== new filename)
 * @param new_text  New note body
 * @param new_emb   Embedding of new_text (dims floats)
 * @param dims      Embedding dimensions
 * @param norm      L2 norm of new_emb
 * @param new_hash  SHA-256 hex of new_text (caller-computed)
 * @return SUCCESS (0) on success, FAILURE (1) if not a note / not owned / error
 */
int document_db_note_update(int user_id,
                            int64_t doc_id,
                            const char *new_label,
                            const char *new_text,
                            const float *new_emb,
                            int dims,
                            float norm,
                            const char *new_hash);

/**
 * @brief Find a document by EXACT (case-insensitive) label/filename (v61).
 *
 * Unlike document_db_find_by_name (substring LIKE), this matches the whole name
 * exactly, so "Bio" never resolves to "Public Bio".  Used for note-overwrite
 * routing and delete-by-label.
 *
 * @param user_id   Scope (own docs + global)
 * @param label     Exact filename/label to match (COLLATE NOCASE)
 * @param note_only If true, restrict to filetype 'note'
 * @param[out] out  Matched document
 * @return SUCCESS (0) if found, FAILURE (1) if not found / error
 */
int document_db_find_by_label_exact(int user_id,
                                    const char *label,
                                    bool note_only,
                                    document_t *out);

/** Kind filter for document_db_list_filtered (v61). */
typedef enum {
   DOC_KIND_ALL = 0,   /**< notes + documents */
   DOC_KIND_DOCS = 1,  /**< documents only (filetype != 'note') */
   DOC_KIND_NOTES = 2, /**< notes only */
} doc_kind_t;

/**
 * @brief List documents filtered by kind (v61).  Paginated; own docs + global.
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_list_filtered(int user_id,
                              doc_kind_t kind,
                              document_t *out,
                              int limit,
                              int offset,
                              int *count_out);

/**
 * @brief Count documents owned by a specific user (excludes global)
 *
 * @param user_id   User ID
 * @param[out] count_out Number of documents (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_count_user(int user_id, int *count_out);

/* =============================================================================
 * Chunk CRUD
 * ============================================================================= */

/**
 * @brief Create a chunk for a document
 *
 * @param document_id Parent document
 * @param chunk_index Order within document (0-based)
 * @param text Chunk text
 * @param embedding Float vector (copied as BLOB)
 * @param dims Number of dimensions
 * @param embedding_norm Pre-computed L2 norm
 * @param created_at Chunk origin timestamp (0 = unknown)
 * @param[out] id_out Created chunk ID (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_chunk_create(int64_t document_id,
                             int chunk_index,
                             const char *text,
                             const float *embedding,
                             int dims,
                             float embedding_norm,
                             int64_t created_at,
                             int64_t *id_out);

/**
 * @brief Find a document by name (exact match preferred, then partial)
 *
 * Searches documents accessible to the user (own + global).
 * If name is all digits, tries ID lookup first.
 *
 * @param user_id User ID
 * @param name Document name or partial name to search for
 * @param out Output document struct
 * @return SUCCESS (0) on success, FAILURE (1) on not found/error
 */
int document_db_find_by_name(int user_id, const char *name, document_t *out);

/**
 * @brief Read chunks from a document in order (paginated)
 *
 * @param document_id Document ID
 * @param chunks Output array (caller allocates, only chunk_index and text populated)
 * @param max_count Maximum chunks to read
 * @param start_chunk Starting chunk index (offset)
 * @param[out] count_out Number of chunks read (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_chunk_read(int64_t document_id,
                           document_chunk_t *chunks,
                           int max_count,
                           int start_chunk,
                           int *count_out);

/**
 * @brief Read a document's chunks whose chunk_index is in [lo_idx, hi_idx], in order.
 *
 * Windows by chunk_index VALUE, not row offset — gap-safe when the index pipeline
 * skipped an index on embed failure.  Only chunk_index and text are populated.
 * Used to expand context around a search/grep hit.
 *
 * @param document_id Document ID
 * @param lo_idx Lowest chunk_index to include (clamped to >= 0 by the caller)
 * @param hi_idx Highest chunk_index to include
 * @param chunks Output array (caller allocates)
 * @param max_count Capacity of @p chunks
 * @param[out] count_out Number of chunks read (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_chunk_read_range(int64_t document_id,
                                 int lo_idx,
                                 int hi_idx,
                                 document_chunk_t *chunks,
                                 int max_count,
                                 int *count_out);

/**
 * @brief Literal substring search over chunk text, scoped to the user's docs + global.
 *
 * Deterministic "grep" — no embeddings, no ranking. Results are ordered by
 * (document_id, chunk_index). Supports paging via @p offset; @p more_out reports
 * whether matches exist beyond this page (detected by fetching one extra row).
 *
 * @param user_id User scope (own docs + global)
 * @param needle Literal substring to match (not a pattern; wildcards are literal)
 * @param case_sensitive false = ASCII case-insensitive, true = exact
 * @param offset Number of leading matches to skip (pagination)
 * @param hits Output array (caller allocates)
 * @param max_count Capacity of @p hits (page size)
 * @param[out] count_out Number of hits returned (must not be NULL)
 * @param[out] more_out Set true if more matches exist past this page (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_chunk_grep(int user_id,
                           const char *needle,
                           bool case_sensitive,
                           int offset,
                           doc_grep_hit_t *hits,
                           int max_count,
                           int *count_out,
                           bool *more_out);

/**
 * @brief Load all chunks accessible to a user for vector search
 *
 * Caller must provide embedding_buf with enough space for max_count * dims floats.
 * Each chunk's embedding pointer is set into embedding_buf.
 *
 * @param user_id User ID (loads own docs + global)
 * @param chunks Output array
 * @param embedding_buf Flat float buffer for embeddings
 * @param dims Expected embedding dimensions
 * @param max_count Maximum chunks to load
 * @param[out] count_out Number of chunks loaded (must not be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on error
 */
int document_db_chunk_search_load(int user_id,
                                  document_chunk_t *chunks,
                                  float *embedding_buf,
                                  int dims,
                                  int max_count,
                                  int *count_out);

/**
 * @brief Lexical (BM25) chunk search — the v61 keyword candidate set.
 *
 * Runs the column-weighted FTS5 bm25() query over document_chunks_fts and
 * returns its OWN ranked candidates (NOT a re-rank of the semantic top-K), so a
 * semantically-buried but lexically-matching chunk still surfaces.  Scores are
 * sigmoid-normalized to [0, 1] (memory_bm25_normalize) for fusion with cosine.
 *
 * @param user_id      User scope (own docs + global)
 * @param query        Raw query text (stemmed internally)
 * @param label_weight BM25 weight for the label/filename column (e.g. 3.0)
 * @param body_weight  BM25 weight for the chunk-text column (e.g. 1.0)
 * @param out          [out] Hit array (caller-allocated, max_hits entries)
 * @param out_scores   [out] Normalized [0,1] score per hit (parallel array)
 * @param max_hits     Capacity of out / out_scores
 * @param[out] count_out Number of hits written (must not be NULL)
 * @return SUCCESS (0) on success (incl. 0 hits / FTS not yet migrated), FAILURE (1) on error
 */
int document_db_chunk_search_bm25(int user_id,
                                  const char *query,
                                  float label_weight,
                                  float body_weight,
                                  doc_bm25_hit_t *out,
                                  float *out_scores,
                                  int max_hits,
                                  int *count_out);

#ifdef __cplusplus
}
#endif

#endif /* DOCUMENT_DB_H */
