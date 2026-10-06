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
 * BERT WordPiece tokenizer with FNV-1a chained hash table.  Reference-counted
 * so multiple ONNX BERT consumers can share one vocab table without each
 * loading its own copy of the ~4.5 MB hash structure.
 */

#include "memory/memory_embed_tokenizer.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_embed_unicode.h"
#include "utils/string_utils.h"

#define VOCAB_HASH_SIZE 65536 /* ~30K entries, 0.46 load factor */
#define VOCAB_MAX_WORD_LEN 128

typedef struct vocab_entry {
   char word[VOCAB_MAX_WORD_LEN];
   int token_id;
   struct vocab_entry *next;
} vocab_entry_t;

static vocab_entry_t *s_vocab_table[VOCAB_HASH_SIZE];
static int s_vocab_size = 0;
static int s_refcount = 0;
static char s_loaded_path[512] = { 0 };
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* FNV-1a */
static uint32_t vocab_hash(const char *str) {
   uint32_t h = 2166136261u;
   for (; *str; str++) {
      h ^= (uint8_t)*str;
      h *= 16777619u;
   }
   return h & (VOCAB_HASH_SIZE - 1);
}

static int vocab_lookup(const char *word) {
   uint32_t idx = vocab_hash(word);
   vocab_entry_t *e = s_vocab_table[idx];
   while (e) {
      if (strcmp(e->word, word) == 0)
         return e->token_id;
      e = e->next;
   }
   return -1;
}

/* Caller holds s_lock. */
static int vocab_load_locked(const char *path) {
   FILE *fp = fopen(path, "r");
   if (!fp) {
      OLOG_ERROR("memory_embed_tokenizer: cannot open vocab: %s", path);
      return FAILURE;
   }

   memset(s_vocab_table, 0, sizeof(s_vocab_table));

   char line[VOCAB_MAX_WORD_LEN];
   int id = 0;
   int truncated_count = 0;
   while (fgets(line, sizeof(line), fp)) {
      size_t len = strlen(line);
      bool had_newline = (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'));
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';

      /* fgets fills the buffer when a line exceeds VOCAB_MAX_WORD_LEN-1.
       * The token would be loaded as a partial string — gibberish lookups
       * later.  Skip it and warn so vocab/model mismatches surface early. */
      if (!had_newline && len == VOCAB_MAX_WORD_LEN - 1 && !feof(fp)) {
         /* Drain the rest of the long line. */
         int ch;
         while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            /* discard */
         }
         truncated_count++;
         id++;
         continue;
      }

      if (len == 0) {
         id++;
         continue;
      }

      uint32_t idx = vocab_hash(line);
      vocab_entry_t *entry = malloc(sizeof(vocab_entry_t));
      if (!entry) {
         OLOG_ERROR("memory_embed_tokenizer: out of memory loading vocab");
         fclose(fp);
         return FAILURE;
      }
      safe_strscpy(entry->word, line);
      entry->token_id = id;
      entry->next = s_vocab_table[idx];
      s_vocab_table[idx] = entry;
      id++;
   }

   fclose(fp);
   s_vocab_size = id;
   if (truncated_count > 0) {
      OLOG_WARNING("memory_embed_tokenizer: skipped %d vocab line(s) >%d chars in %s — "
                   "vocab/model mismatch?",
                   truncated_count, VOCAB_MAX_WORD_LEN - 1, path);
   }
   /* Sanity check: warn if the BERT special-token IDs aren't where we
    * expect them.  Standard BERT vocabs put [CLS]=101, [SEP]=102, [UNK]=100.
    * If the file has stray blank lines or starts with a different layout,
    * IDs will drift and embeddings will be silently wrong. */
   if (s_vocab_size > MEMORY_TOKENIZER_TOKEN_SEP) {
      vocab_entry_t *e_cls = NULL;
      vocab_entry_t *e_sep = NULL;
      uint32_t cls_h = vocab_hash("[CLS]");
      uint32_t sep_h = vocab_hash("[SEP]");
      for (vocab_entry_t *e = s_vocab_table[cls_h]; e; e = e->next)
         if (strcmp(e->word, "[CLS]") == 0) {
            e_cls = e;
            break;
         }
      for (vocab_entry_t *e = s_vocab_table[sep_h]; e; e = e->next)
         if (strcmp(e->word, "[SEP]") == 0) {
            e_sep = e;
            break;
         }
      if (!e_cls || e_cls->token_id != MEMORY_TOKENIZER_TOKEN_CLS || !e_sep ||
          e_sep->token_id != MEMORY_TOKENIZER_TOKEN_SEP) {
         OLOG_WARNING("memory_embed_tokenizer: vocab special-token IDs unexpected "
                      "([CLS]=%d expected %d, [SEP]=%d expected %d) — model mismatch or "
                      "non-standard vocab; embeddings may be wrong",
                      e_cls ? e_cls->token_id : -1, MEMORY_TOKENIZER_TOKEN_CLS,
                      e_sep ? e_sep->token_id : -1, MEMORY_TOKENIZER_TOKEN_SEP);
      }
   }
   OLOG_INFO("memory_embed_tokenizer: loaded vocab with %d entries from %s", s_vocab_size, path);
   return SUCCESS;
}

/* Caller holds s_lock. */
static void vocab_free_locked(void) {
   for (int i = 0; i < VOCAB_HASH_SIZE; i++) {
      vocab_entry_t *e = s_vocab_table[i];
      while (e) {
         vocab_entry_t *next = e->next;
         free(e);
         e = next;
      }
      s_vocab_table[i] = NULL;
   }
   s_vocab_size = 0;
   s_loaded_path[0] = '\0';
}

int memory_embed_tokenizer_acquire(const char *path) {
   if (!path || !*path)
      return FAILURE;

   pthread_mutex_lock(&s_lock);

   if (s_refcount > 0) {
      if (strcmp(s_loaded_path, path) != 0) {
         OLOG_ERROR("memory_embed_tokenizer: refusing to load %s — %s already loaded", path,
                    s_loaded_path);
         pthread_mutex_unlock(&s_lock);
         return FAILURE;
      }
      s_refcount++;
      pthread_mutex_unlock(&s_lock);
      return SUCCESS;
   }

   if (vocab_load_locked(path) != SUCCESS) {
      vocab_free_locked();
      pthread_mutex_unlock(&s_lock);
      return FAILURE;
   }

   safe_strscpy(s_loaded_path, path);
   s_refcount = 1;
   pthread_mutex_unlock(&s_lock);
   return SUCCESS;
}

void memory_embed_tokenizer_release(void) {
   pthread_mutex_lock(&s_lock);
   if (s_refcount <= 0) {
      pthread_mutex_unlock(&s_lock);
      return;
   }
   s_refcount--;
   if (s_refcount == 0) {
      vocab_free_locked();
      OLOG_INFO("memory_embed_tokenizer: vocab unloaded");
   }
   pthread_mutex_unlock(&s_lock);
}

bool memory_embed_tokenizer_available(void) {
   pthread_mutex_lock(&s_lock);
   bool ok = (s_refcount > 0 && s_vocab_size > 0);
   pthread_mutex_unlock(&s_lock);
   return ok;
}

/* Reference WordPiece limit: a longer word becomes a single [UNK]. */
#define WP_MAX_WORD_CHARS 100
/* Input read per call (bytes).  Far more than max_len tokens can use. */
#define WP_MAX_INPUT_BYTES 4096

static int emit_token(int id,
                      int64_t type_id,
                      int64_t *input_ids,
                      int64_t *attention_mask,
                      int64_t *token_type_ids,
                      int pos) {
   input_ids[pos] = id;
   attention_mask[pos] = 1;
   token_type_ids[pos] = type_id;
   return pos + 1;
}

/* Greedy longest-match-first WordPiece over one word's characters, exactly as
 * the reference: if any position has no matching piece the whole word is one
 * [UNK] (never a partial word). */
static int wordpiece_word(const uint32_t *w,
                          int n,
                          bool overlong,
                          int64_t type_id,
                          int64_t *input_ids,
                          int64_t *attention_mask,
                          int64_t *token_type_ids,
                          int pos,
                          int limit) {
   if ((n <= 0 && !overlong) || pos >= limit) {
      return pos;
   }
   if (overlong) {
      return emit_token(MEMORY_TOKENIZER_TOKEN_UNK, type_id, input_ids, attention_mask,
                        token_type_ids, pos);
   }

   /* The word's UTF-8 once, with each character's byte offset, so a candidate
    * piece is a slice rather than a re-encode. */
   char bytes[WP_MAX_WORD_CHARS * 4];
   int off[WP_MAX_WORD_CHARS + 1];
   int nb = 0;
   for (int k = 0; k < n; k++) {
      off[k] = nb;
      nb += embed_utf8_encode(w[k], bytes + nb);
   }
   off[n] = nb;

   int pieces[WP_MAX_WORD_CHARS];
   int n_pieces = 0;
   int start = 0;
   while (start < n) {
      int found = -1;
      int end = n;
      const size_t prefix = (start > 0) ? 2 : 0; /* "##" */
      for (; end > start; end--) {
         const size_t span = (size_t)(off[end] - off[start]);
         if (prefix + span >= VOCAB_MAX_WORD_LEN) {
            continue; /* longer than any vocabulary entry */
         }
         char piece[VOCAB_MAX_WORD_LEN];
         if (prefix) {
            piece[0] = '#';
            piece[1] = '#';
         }
         memcpy(piece + prefix, bytes + off[start], span);
         piece[prefix + span] = '\0';
         int id = vocab_lookup(piece);
         if (id >= 0) {
            found = id;
            break;
         }
      }
      if (found < 0) {
         return emit_token(MEMORY_TOKENIZER_TOKEN_UNK, type_id, input_ids, attention_mask,
                           token_type_ids, pos);
      }
      pieces[n_pieces++] = found;
      start = end;
   }
   for (int i = 0; i < n_pieces && pos < limit; i++) {
      pos = emit_token(pieces[i], type_id, input_ids, attention_mask, token_type_ids, pos);
   }
   return pos;
}

/**
 * @brief Tokenize @p text into WordPiece ids, as the reference uncased BERT
 *        tokenizer does.
 *
 * Pipeline: UTF-8 decode; drop control characters (and NUL / U+FFFD);
 * whitespace separates words; each CJK ideograph is its own word; every other
 * character is lowercased, NFD-decomposed and stripped of combining marks
 * (so "Café" -> "cafe"), and punctuation characters (ASCII or Unicode P*)
 * become single-character words; then WordPiece.  Appends tokens starting at
 * @p pos and stops at @p max_len - @p reserve so the caller can add [SEP]s.
 *
 * @return Updated position
 */
static int wordpiece_append(const char *text,
                            int64_t type_id,
                            int64_t *input_ids,
                            int64_t *attention_mask,
                            int64_t *token_type_ids,
                            int pos,
                            int max_len,
                            int reserve) {
   if (!text)
      return pos;
   const int limit = max_len - reserve;

   size_t len = strlen(text);
   if (len > WP_MAX_INPUT_BYTES)
      len = WP_MAX_INPUT_BYTES;

   uint32_t word[WP_MAX_WORD_CHARS];
   int wn = 0;
   bool overlong = false;

#define FLUSH_WORD()                                                                               \
   do {                                                                                            \
      pos = wordpiece_word(word, wn, overlong, type_id, input_ids, attention_mask, token_type_ids, \
                           pos, limit);                                                            \
      wn = 0;                                                                                      \
      overlong = false;                                                                            \
   } while (0)

   size_t i = 0;
   while (i < len && pos < limit) {
      uint32_t cp;
      i += embed_utf8_decode(text + i, len - i, &cp);
      if (cp == 0 || cp == 0xFFFD || embed_cp_is_control(cp)) {
         continue;
      }
      if (embed_cp_is_space(cp)) {
         FLUSH_WORD();
         continue;
      }
      if (embed_cp_is_cjk(cp)) {
         FLUSH_WORD();
         word[wn++] = cp;
         FLUSH_WORD();
         continue;
      }

      uint32_t folded[EMBED_FOLD_MAX];
      int nf = embed_cp_fold(cp, folded);
      for (int k = 0; k < nf; k++) {
         if (embed_cp_is_punct(folded[k])) {
            FLUSH_WORD();
            word[wn++] = folded[k];
            FLUSH_WORD();
         } else if (wn < WP_MAX_WORD_CHARS) {
            word[wn++] = folded[k];
         } else {
            overlong = true;
         }
      }
   }
   if (pos < limit) {
      FLUSH_WORD();
   }
#undef FLUSH_WORD
   return pos;
}

static void zero_pad(int64_t *input_ids,
                     int64_t *attention_mask,
                     int64_t *token_type_ids,
                     int pos,
                     int max_len) {
   for (int i = pos; i < max_len; i++) {
      input_ids[i] = 0;
      attention_mask[i] = 0;
      token_type_ids[i] = 0;
   }
}

int memory_embed_tokenizer_encode(const char *text,
                                  int64_t *input_ids,
                                  int64_t *attention_mask,
                                  int64_t *token_type_ids,
                                  int max_len) {
   if (!text || !input_ids || !attention_mask || !token_type_ids || max_len < 3)
      return 0;

   int pos = 0;
   input_ids[pos] = MEMORY_TOKENIZER_TOKEN_CLS;
   attention_mask[pos] = 1;
   token_type_ids[pos] = 0;
   pos++;

   /* Reserve 1 slot for trailing [SEP]. */
   pos = wordpiece_append(text, /* type_id */ 0, input_ids, attention_mask, token_type_ids, pos,
                          max_len, /* reserve */ 1);

   if (pos < max_len) {
      input_ids[pos] = MEMORY_TOKENIZER_TOKEN_SEP;
      attention_mask[pos] = 1;
      token_type_ids[pos] = 0;
      pos++;
   }

   zero_pad(input_ids, attention_mask, token_type_ids, pos, max_len);
   return pos;
}

int memory_embed_tokenizer_encode_pair(const char *query,
                                       const char *passage,
                                       int64_t *input_ids,
                                       int64_t *attention_mask,
                                       int64_t *token_type_ids,
                                       int max_len) {
   if (!query || !input_ids || !attention_mask || !token_type_ids || max_len < 4)
      return 0;

   int pos = 0;
   input_ids[pos] = MEMORY_TOKENIZER_TOKEN_CLS;
   attention_mask[pos] = 1;
   token_type_ids[pos] = 0;
   pos++;

   /* Query: keep whole, reserve 3 slots ([SEP] passage [SEP], minimum). */
   pos = wordpiece_append(query, /* type_id */ 0, input_ids, attention_mask, token_type_ids, pos,
                          max_len, /* reserve */ 3);

   if (pos < max_len) {
      input_ids[pos] = MEMORY_TOKENIZER_TOKEN_SEP;
      attention_mask[pos] = 1;
      token_type_ids[pos] = 0;
      pos++;
   }

   /* Passage: token_type_ids = 1, truncate from the right. */
   if (passage) {
      pos = wordpiece_append(passage, /* type_id */ 1, input_ids, attention_mask, token_type_ids,
                             pos, max_len, /* reserve */ 1);
   }

   if (pos < max_len) {
      input_ids[pos] = MEMORY_TOKENIZER_TOKEN_SEP;
      attention_mask[pos] = 1;
      token_type_ids[pos] = 1;
      pos++;
   }

   zero_pad(input_ids, attention_mask, token_type_ids, pos, max_len);
   return pos;
}
