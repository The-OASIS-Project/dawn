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
 * Shared WordPiece Tokenizer
 *
 * BERT WordPiece tokenizer used by ONNX BERT models (bge-small bi-encoder
 * today; designed to also serve future cross-encoder consumers).  One vocab
 * hash table is loaded once and shared via reference counting.
 */

#ifndef MEMORY_EMBED_TOKENIZER_H
#define MEMORY_EMBED_TOKENIZER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard BERT special token IDs (shared across MiniLM and bge-small). */
#define MEMORY_TOKENIZER_TOKEN_CLS 101
#define MEMORY_TOKENIZER_TOKEN_SEP 102
#define MEMORY_TOKENIZER_TOKEN_UNK 100

/* Revision of the tokenizer's output.  Bump whenever a change alters the token
 * ids produced for any input: stored ONNX embeddings were computed with the old
 * ids, and the embedding re-index keys on this (memory_embed_recompute.c) so
 * they are rebuilt once on upgrade.
 *   1 = ASCII-only pre-tokenization
 *   2 = reference BERT (Unicode) pre-tokenization and WordPiece */
#define MEMORY_EMBED_TOKENIZER_REVISION 2

/**
 * @brief Acquire a reference to the shared vocab table.
 *
 * Loads the vocabulary from @p path on the first call. Subsequent calls
 * with the same path increment a reference count and return SUCCESS without
 * reloading. Calls with a different path while one is loaded return FAILURE.
 *
 * Path identity is checked by byte-for-byte strcmp; equivalent paths that
 * differ in spelling (symlinks, relative vs absolute) are not reconciled.
 *
 * Threading: acquire/release/encode all share one internal mutex on the
 * refcount + vocab table.  Callers MUST ensure no encode call is in flight
 * when calling release — encode reads the vocab table without locking, and
 * a concurrent release that drops the refcount to zero would free it from
 * under the reader.  The current design relies on acquire/release happening
 * only at module init/shutdown (single-threaded contexts).
 *
 * @param path Path to vocab.txt (one token per line, ID = line number;
 *             blank lines reserve an unused ID slot — standard BERT
 *             convention used by ms-marco / bge / minilm vocabs)
 * @return SUCCESS or FAILURE
 */
int memory_embed_tokenizer_acquire(const char *path);

/**
 * @brief Release a reference to the shared vocab table.
 *
 * Frees the vocab when the reference count drops to zero.
 */
void memory_embed_tokenizer_release(void);

/**
 * @brief Check if the tokenizer is loaded.
 */
bool memory_embed_tokenizer_available(void);

/**
 * @brief Tokenize a single sentence: [CLS] text [SEP]
 *
 * All token_type_ids are set to 0. Output arrays are zero-padded out to
 * @p max_len.
 *
 * @param text Input text
 * @param input_ids Output token IDs (length @p max_len)
 * @param attention_mask Output attention mask (length @p max_len)
 * @param token_type_ids Output token type IDs (length @p max_len)
 * @param max_len Buffer length
 * @return Number of real tokens produced (including [CLS] and [SEP])
 */
int memory_embed_tokenizer_encode(const char *text,
                                  int64_t *input_ids,
                                  int64_t *attention_mask,
                                  int64_t *token_type_ids,
                                  int max_len);

/**
 * @brief Tokenize a (query, passage) pair: [CLS] query [SEP] passage [SEP]
 *
 * Query tokens get token_type_ids = 0, passage tokens (and the trailing
 * [SEP]) get 1. The query is kept whole; the passage is truncated from the
 * right if the combined sequence would exceed @p max_len. Output arrays are
 * zero-padded out to @p max_len.
 *
 * @param query Query text (not truncated)
 * @param passage Passage text (truncated from the right if needed)
 * @param input_ids Output token IDs (length @p max_len)
 * @param attention_mask Output attention mask (length @p max_len)
 * @param token_type_ids Output token type IDs (length @p max_len)
 * @param max_len Buffer length (must be >= 4 for [CLS] + 1 query + [SEP] + [SEP])
 * @return Number of real tokens produced
 */
int memory_embed_tokenizer_encode_pair(const char *query,
                                       const char *passage,
                                       int64_t *input_ids,
                                       int64_t *attention_mask,
                                       int64_t *token_type_ids,
                                       int max_len);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EMBED_TOKENIZER_H */
