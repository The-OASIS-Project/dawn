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
 * Hybrid ranking of a user's document chunks.  See document_rank.h.
 *
 * Ranking works on chunk coordinates and labels only; text is fetched for the
 * chunks a caller keeps (and, for the body phrase bonus, the ones it's scored
 * on), so a turn that injects nothing reads no chunk text.
 */

#include "tools/document_rank.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/time_query_parser.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_stem.h"

_Static_assert(DOCUMENT_RANK_LEXICAL_MAX <= DOCUMENT_RANK_WANT_MAX,
               "every keyword hit's cosine is requested from the semantic pass");

/* Words of a text the phrase bonus compares (longer texts are cut here). */
#define DOC_RANK_PHRASE_MAX_TOKENS 256
/* Query words the phrase bonus looks for. */
#define DOC_RANK_PHRASE_MAX_WORDS 32
/* Room for a query's content words, joined for one stemming pass. */
#define DOC_RANK_QUERY_WORDS_MAX (DOCUMENT_QUERY_TERMS_MAX * DOCUMENT_QUERY_TERM_LEN)

/* What a ranking's items point into. */
struct document_ranking_store {
   doc_chunk_meta_t *metas;  /* the semantic top */
   doc_bm25_hit_t *hits;     /* keyword hits */
   document_chunk_t **texts; /* blocks of loaded text */
   int n_texts;
};

/* =============================================================================
 * Words
 *
 * One definition of a word for query terms, label terms and the phrase bonus:
 * a run of letters and digits, ASCII or not (so "résumé" is one word and
 * "Tax_Return_2024" three).  Punctuation and spaces outside ASCII — curly
 * quotes, dashes, the ellipsis, a no-break space — separate words like their
 * ASCII counterparts, so a phone keyboard's “marigold project plan” is three words.  The
 * query and the label must split the same way or their words can't match.
 * ============================================================================= */

/* Byte length of the separator at @p p: ASCII other than a letter or digit,
 * or a UTF-8 punctuation/space character; 0 when @p p starts a word character. */
static size_t separator_len(const unsigned char *p) {
   if (p[0] < 0x80) {
      return isalnum(p[0]) ? 0 : 1;
   }
   if (p[0] >= 0xC2 && p[0] <= 0xDF && p[1] >= 0x80 && p[1] <= 0xBF) {
      const unsigned cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
      /* Latin-1 punctuation and symbols (no-break space, «», ¿, ...), × and ÷,
       * but not the letters and digits in that block (ª µ º ¹ ² ³ ¼ ½ ¾). */
      const bool letter = cp == 0xAA || cp == 0xB2 || cp == 0xB3 || cp == 0xB5 || cp == 0xB9 ||
                          cp == 0xBA || (cp >= 0xBC && cp <= 0xBE);
      return ((cp <= 0xBF && !letter) || cp == 0xD7 || cp == 0xF7) ? 2 : 0;
   }
   if (p[0] >= 0xE0 && p[0] <= 0xEF && p[1] >= 0x80 && p[1] <= 0xBF && p[2] >= 0x80 &&
       p[2] <= 0xBF) {
      const unsigned cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
      /* General punctuation (spaces, dashes, quotes, ellipsis), supplemental
       * punctuation, CJK punctuation (not its letters and numerals, U+3005-3007
       * and U+3021-303C), and the byte-order mark. */
      const bool cjk_punct = (cp >= 0x3000 && cp <= 0x3004) || (cp >= 0x3008 && cp <= 0x3020) ||
                             (cp >= 0x303D && cp <= 0x303F);
      const bool punct = (cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x2E00 && cp <= 0x2E7F) ||
                         cjk_punct || cp == 0xFEFF;
      return punct ? 3 : 0;
   }
   return 0; /* other letters, and malformed bytes, stay in the word */
}

/* The next word at or after @p p, or NULL at the end of the text. */
static const char *next_word(const char *p, const char **start, size_t *len) {
   size_t sep = 0;
   while (*p && (sep = separator_len((const unsigned char *)p)) > 0) {
      p += sep;
   }
   if (!*p) {
      return NULL;
   }
   *start = p;
   while (*p && separator_len((const unsigned char *)p) == 0) {
      p++;
   }
   *len = (size_t)(p - *start);
   return p;
}

/* Byte @p i of word @p w, case-folded: ASCII, and the Latin-1 capitals À–Þ
 * (UTF-8 C3 80–9E, other than ×), which fold to à–þ by adding 0x20. */
static unsigned char fold_at(const char *w, size_t i) {
   const unsigned char c = (unsigned char)w[i];
   if (c < 0x80) {
      return (unsigned char)tolower(c);
   }
   if (i > 0 && (unsigned char)w[i - 1] == 0xC3 && c >= 0x80 && c <= 0x9E && c != 0x97) {
      return (unsigned char)(c + 0x20);
   }
   return c;
}

/* Function words that say nothing about which document is meant.  Kept short
 * on purpose: a word missing here costs little (a label must still contain two
 * query words), a content word added here would be ignored.  The codebase's
 * other lists serve other purposes (the dominant-token list drops words under
 * four letters, which would lose label words like "tax" and "vet"). */
static const char *const QUERY_STOPWORDS[] = {
   "a",    "about", "all",   "am",    "an",    "and",    "any",   "are",    "as",    "at",   "be",
   "been", "but",   "by",    "can",   "could", "did",    "do",    "does",   "for",   "from", "get",
   "had",  "has",   "have",  "he",    "her",   "him",    "his",   "how",    "i",     "if",   "in",
   "into", "is",    "it",    "its",   "just",  "know",   "let",   "me",     "my",    "no",   "not",
   "of",   "on",    "or",    "our",   "out",   "please", "she",   "should", "so",    "some", "tell",
   "than", "that",  "the",   "their", "them",  "then",   "there", "these",  "they",  "this", "to",
   "up",   "us",    "was",   "we",    "were",  "what",   "when",  "where",  "which", "who",  "why",
   "will", "with",  "would", "you",   "your",
};

static bool is_query_stopword(const char *word) {
   for (size_t i = 0; i < sizeof(QUERY_STOPWORDS) / sizeof(QUERY_STOPWORDS[0]); i++) {
      if (strcmp(QUERY_STOPWORDS[i], word) == 0) {
         return true;
      }
   }
   return false;
}

/* Append @p text's words (lowercased, content words only when @p content) to
 * @p buf, space-separated, stopping at @p max_words.  Returns words added. */
static int join_words(const char *text, bool content, int max_words, char *buf, size_t size) {
   size_t off = 0;
   int n = 0;
   const char *start = NULL;
   size_t len = 0;
   buf[0] = '\0';
   for (const char *p = text; n < max_words && (p = next_word(p, &start, &len)) != NULL;) {
      if (len < 2 || len >= DOCUMENT_QUERY_TERM_LEN || off + len + 2 > size) {
         continue; /* single letters, runs too long to be a word, or no room */
      }
      char word[DOCUMENT_QUERY_TERM_LEN];
      for (size_t i = 0; i < len; i++) {
         word[i] = (char)fold_at(start, i);
      }
      word[len] = '\0';
      if (content && is_query_stopword(word)) {
         continue;
      }
      if (off > 0) {
         buf[off++] = ' ';
      }
      memcpy(buf + off, word, len + 1);
      off += len;
      n++;
   }
   return n;
}

void document_query_terms(const char *query, document_query_terms_t *out) {
   out->count = 0;
   if (!query) {
      return;
   }
   char words[DOC_RANK_QUERY_WORDS_MAX];
   char stems[DOC_RANK_QUERY_WORDS_MAX];
   /* Stem them in one pass: one hold of the stemmer's lock, not one per word. */
   if (join_words(query, true, DOCUMENT_QUERY_TERMS_MAX, words, sizeof(words)) == 0 ||
       memory_stem_string(words, stems, sizeof(stems)) <= 0) {
      return;
   }
   char *save = NULL;
   for (char *tok = strtok_r(stems, " ", &save); tok && out->count < DOCUMENT_QUERY_TERMS_MAX;
        tok = strtok_r(NULL, " ", &save)) {
      bool seen = strlen(tok) >= DOCUMENT_QUERY_TERM_LEN;
      for (int i = 0; i < out->count && !seen; i++) {
         seen = strcmp(out->term[i], tok) == 0;
      }
      if (!seen) {
         memcpy(out->term[out->count], tok, strlen(tok) + 1);
         out->count++;
      }
   }
}

int document_label_terms(const document_query_terms_t *terms, const char *label) {
   if (!terms || terms->count == 0 || !label || !label[0]) {
      return 0;
   }
   char words[DOC_FILENAME_MAX];
   char stems[DOC_FILENAME_MAX];
   if (join_words(label, false, DOC_FILENAME_MAX, words, sizeof(words)) == 0 ||
       memory_stem_string(words, stems, sizeof(stems)) <= 0) {
      return 0;
   }
   bool found[DOCUMENT_QUERY_TERMS_MAX] = { false };
   int count = 0;
   char *save = NULL;
   for (char *tok = strtok_r(stems, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
      for (int i = 0; i < terms->count; i++) {
         if (!found[i] && strcmp(terms->term[i], tok) == 0) {
            found[i] = true;
            count++;
         }
      }
   }
   return count;
}

bool document_label_matches(int label_terms, int query_terms) {
   if (query_terms <= 0) {
      return false;
   }
   const int needed = query_terms < 2 ? query_terms : 2;
   return label_terms >= needed;
}

/* =============================================================================
 * Phrase bonus
 *
 * BM25 is bag-of-words; this adds "how many query words appear, and in order".
 * Returns max(ordered_ratio, contiguous_ratio) in [0, 1]: the fraction of query
 * words found in the text in query order, and the longest run of query words
 * back to back, over the query length.  An exact label phrase yields 1.0.
 * ============================================================================= */

typedef struct {
   const char *words[DOC_RANK_PHRASE_MAX_WORDS];
   int lengths[DOC_RANK_PHRASE_MAX_WORDS];
   int count;
} phrase_words_t;

static void phrase_words(const char *query, phrase_words_t *qw) {
   qw->count = 0;
   const char *start = NULL;
   size_t len = 0;
   for (const char *p = query;
        qw->count < DOC_RANK_PHRASE_MAX_WORDS && (p = next_word(p, &start, &len)) != NULL;) {
      if (len >= 3) { /* skip very short words */
         qw->words[qw->count] = start;
         qw->lengths[qw->count] = (int)len;
         qw->count++;
      }
   }
}

static bool tok_eq_ci(const char *a, int al, const char *b, int bl) {
   if (al != bl) {
      return false;
   }
   for (int i = 0; i < al; i++) {
      if (fold_at(a, (size_t)i) != fold_at(b, (size_t)i)) {
         return false;
      }
   }
   return true;
}

static float phrase_bonus(const char *text, const phrase_words_t *qw) {
   if (!text || qw->count == 0) {
      return 0.0f;
   }
   const char *tstart[DOC_RANK_PHRASE_MAX_TOKENS];
   int tlen[DOC_RANK_PHRASE_MAX_TOKENS];
   int nt = 0;
   const char *start = NULL;
   size_t len = 0;
   for (const char *p = text;
        nt < DOC_RANK_PHRASE_MAX_TOKENS && (p = next_word(p, &start, &len)) != NULL;) {
      tstart[nt] = start;
      tlen[nt] = (int)len;
      nt++;
   }
   if (nt == 0) {
      return 0.0f;
   }
   const int nq = qw->count;

   /* Ordered: greedy in-order match of query words against the text. */
   int ordered = 0;
   int ti = 0;
   for (int qi = 0; qi < nq; qi++) {
      for (int p = ti; p < nt; p++) {
         if (tok_eq_ci(tstart[p], tlen[p], qw->words[qi], qw->lengths[qi])) {
            ordered++;
            ti = p + 1;
            break;
         }
      }
   }

   /* Contiguous: longest run of consecutive query words back to back. */
   int best_run = 0;
   for (int ts = 0; ts < nt; ts++) {
      for (int qs = 0; qs < nq; qs++) {
         int run = 0;
         while (ts + run < nt && qs + run < nq &&
                tok_eq_ci(tstart[ts + run], tlen[ts + run], qw->words[qs + run],
                          qw->lengths[qs + run])) {
            run++;
         }
         if (run > best_run) {
            best_run = run;
         }
      }
   }

   const float ordered_ratio = (float)ordered / (float)nq;
   const float contiguous_ratio = (float)best_run / (float)nq;
   return ordered_ratio > contiguous_ratio ? ordered_ratio : contiguous_ratio;
}

/* =============================================================================
 * Ranking
 * ============================================================================= */

static int cmp_cosine(const void *a, const void *b) {
   const float ca = ((const document_ranked_t *)a)->cosine;
   const float cb = ((const document_ranked_t *)b)->cosine;
   return (cb > ca) - (cb < ca);
}

static int cmp_hybrid(const void *a, const void *b) {
   const float ha = ((const document_ranked_t *)a)->hybrid;
   const float hb = ((const document_ranked_t *)b)->hybrid;
   return (hb > ha) - (hb < ha);
}

void document_ranking_free(document_ranking_t *ranking) {
   if (!ranking) {
      return;
   }
   struct document_ranking_store *st = ranking->store;
   if (st) {
      free(st->metas);
      free(st->hits);
      for (int i = 0; i < st->n_texts; i++) {
         free(st->texts[i]);
      }
      free(st->texts);
      free(st);
   }
   free(ranking->items);
   memset(ranking, 0, sizeof(*ranking));
}

int document_ranking_load_text(int user_id,
                               document_ranking_t *ranking,
                               document_ranked_t *const *items,
                               int n) {
   if (!ranking || !ranking->store || (n > 0 && !items)) {
      return FAILURE;
   }
   int64_t ids[DOCUMENT_RANK_MAX + DOCUMENT_RANK_LEXICAL_MAX];
   int n_ids = 0;
   for (int i = 0; i < n && n_ids < (int)(sizeof(ids) / sizeof(ids[0])); i++) {
      if (!items[i]->text) {
         ids[n_ids++] = items[i]->chunk_id;
      }
   }
   if (n_ids == 0) {
      return SUCCESS;
   }
   struct document_ranking_store *st = ranking->store;
   document_chunk_t **texts = realloc(st->texts, (size_t)(st->n_texts + 1) * sizeof(*texts));
   if (!texts) {
      return FAILURE;
   }
   st->texts = texts;
   document_chunk_t *block = malloc((size_t)n_ids * sizeof(*block));
   int loaded = 0;
   if (!block || document_db_chunks_get_by_ids(user_id, ids, n_ids, block, &loaded) != SUCCESS) {
      free(block);
      return FAILURE;
   }
   st->texts[st->n_texts++] = block;
   for (int j = 0; j < loaded; j++) {
      for (int i = 0; i < n; i++) {
         if (!items[i]->text && items[i]->chunk_id == block[j].id) {
            items[i]->text = block[j].text;
         }
      }
   }
   return SUCCESS;
}

/* Keyword channel.  A search failure leaves the semantic channel ranking. */
static int rank_keyword(int user_id,
                        const char *query,
                        const document_rank_opts_t *opts,
                        struct document_ranking_store *st,
                        float *scores) {
   if (opts->lexical_limit <= 0) {
      return 0;
   }
   st->hits = malloc((size_t)opts->lexical_limit * sizeof(*st->hits));
   int n = 0;
   const documents_config_t *dc = &g_config.documents;
   if (!st->hits ||
       document_db_chunk_search_bm25(user_id, query, dc->fts_label_weight, dc->fts_body_weight,
                                     st->hits, scores, opts->lexical_limit, &n) != SUCCESS) {
      OLOG_WARNING("document_rank: keyword search failed (user_id=%d); semantic only", user_id);
      return 0;
   }
   return n;
}

/* Semantic channel: the top chunks by cosine (coordinates only) and, from the
 * same pass, the cosine of each keyword hit (NAN when it has none). */
static int rank_semantic(int user_id,
                         const float *qvec,
                         int dims,
                         const document_rank_opts_t *opts,
                         document_ranking_t *r,
                         int n_hits,
                         float *hit_cos,
                         document_chunk_score_t *top,
                         int *n_top,
                         int *n_meta) {
   struct document_ranking_store *st = r->store;
   int64_t hit_ids[DOCUMENT_RANK_LEXICAL_MAX];
   for (int i = 0; i < n_hits; i++) {
      hit_ids[i] = st->hits[i].id;
   }
   if (document_embed_rank_with(user_id, qvec, dims, opts->semantic_top, top, n_top, &r->stats,
                                hit_ids, n_hits, hit_cos) != SUCCESS) {
      return FAILURE;
   }
   if (*n_top == 0) {
      return SUCCESS;
   }
   st->metas = malloc((size_t)*n_top * sizeof(*st->metas));
   if (!st->metas) {
      return FAILURE;
   }
   int64_t ids[DOCUMENT_RANK_MAX];
   for (int i = 0; i < *n_top; i++) {
      ids[i] = top[i].chunk_id;
   }
   return document_db_chunks_meta_by_ids(user_id, ids, *n_top, st->metas, n_meta);
}

static document_ranked_t *add_item(document_ranking_t *r, const doc_chunk_meta_t *m) {
   document_ranked_t *it = &r->items[r->count++];
   it->chunk_id = m->id;
   it->document_id = m->document_id;
   it->chunk_index = m->chunk_index;
   it->created_at = m->created_at;
   it->filename = m->filename;
   return it;
}

/* Candidates: the semantic top (as loaded), then keyword hits not among them. */
static void merge_candidates(document_ranking_t *r,
                             const document_chunk_score_t *top,
                             int n_top,
                             int n_meta,
                             int n_hits,
                             const float *hit_bm25,
                             const float *hit_cos) {
   const struct document_ranking_store *st = r->store;
   for (int i = 0; i < n_meta; i++) {
      document_ranked_t *it = add_item(r, &st->metas[i]);
      for (int t = 0; t < n_top; t++) {
         if (top[t].chunk_id == it->chunk_id) {
            it->cosine = top[t].cosine;
            it->has_cosine = true;
            break;
         }
      }
   }
   const int n_semantic = r->count;
   for (int j = 0; j < n_hits; j++) {
      document_ranked_t *it = NULL;
      for (int k = 0; k < n_semantic; k++) {
         if (r->items[k].chunk_id == st->hits[j].id) {
            it = &r->items[k];
            break;
         }
      }
      if (!it) {
         it = add_item(r, &st->hits[j]);
         if (!isnan(hit_cos[j])) {
            it->cosine = hit_cos[j];
            it->has_cosine = true;
         }
      }
      it->bm25 = hit_bm25[j];
   }
}

/* Fused score: vec * cosine + kw * bm25 (+ proximity to a time in the query). */
static void score_candidates(document_ranking_t *r, const char *query, bool temporal) {
   const documents_config_t *dc = &g_config.documents;
   time_query_t tq = { 0 };
   const float temporal_weight = g_config.memory.temporal_weight;
   if (temporal && temporal_weight > 0.0f) {
      time_query_parse(query, (int64_t)time(NULL), &tq);
   }
   for (int i = 0; i < r->count; i++) {
      document_ranked_t *it = &r->items[i];
      float h = dc->hybrid_vector_weight * it->cosine + dc->hybrid_keyword_weight * it->bm25;
      if (tq.found && it->created_at > 0) {
         h += temporal_weight * time_query_proximity(&tq, it->created_at);
      }
      it->hybrid = h;
   }
}

/* Phrase bonus over the best by cosine plus every keyword hit, so a note whose
 * label matches still gets it when its cosine ranks low. */
static void add_phrase_bonus(int user_id,
                             document_ranking_t *r,
                             const char *query,
                             const document_rank_opts_t *opts) {
   const float weight = g_config.documents.phrase_bonus_weight;
   phrase_words_t qw;
   phrase_words(query, &qw);
   if (weight <= 0.0f || qw.count == 0) {
      return;
   }
   qsort(r->items, (size_t)r->count, sizeof(*r->items), cmp_cosine);
   document_ranked_t *eligible[DOCUMENT_RANK_MAX + DOCUMENT_RANK_LEXICAL_MAX];
   int n = 0;
   for (int i = 0; i < r->count; i++) {
      if (i < opts->phrase_top || r->items[i].bm25 > 0.0f) {
         eligible[n++] = &r->items[i];
      }
   }
   if (opts->body_phrase && document_ranking_load_text(user_id, r, eligible, n) != SUCCESS) {
      OLOG_WARNING("document_rank: couldn't load text for the phrase bonus; labels only");
   }
   for (int i = 0; i < n; i++) {
      /* The label phrase is the strongest exact-retrieval signal: take the max. */
      const float label = phrase_bonus(eligible[i]->filename, &qw);
      const float body = opts->body_phrase ? phrase_bonus(eligible[i]->text, &qw) : 0.0f;
      eligible[i]->hybrid += weight * (label > body ? label : body);
   }
}

/* Label words per document, computed once per document rather than per chunk. */
static void set_label_terms(document_ranking_t *r, const document_query_terms_t *terms) {
   for (int i = 0; i < r->count; i++) {
      document_ranked_t *it = &r->items[i];
      it->label_terms = -1;
      for (int j = 0; j < i; j++) {
         if (r->items[j].document_id == it->document_id) {
            it->label_terms = r->items[j].label_terms;
            break;
         }
      }
      if (it->label_terms < 0) {
         it->label_terms = document_label_terms(terms, it->filename);
      }
   }
}

int document_rank_hybrid(int user_id,
                         const char *query,
                         const float *qvec,
                         int dims,
                         const document_rank_opts_t *opts,
                         document_ranking_t *out) {
   if (!out) {
      return FAILURE;
   }
   memset(out, 0, sizeof(*out));
   if (!query || !opts || opts->semantic_top <= 0 || opts->semantic_top > DOCUMENT_RANK_MAX ||
       opts->lexical_limit < 0 || opts->lexical_limit > DOCUMENT_RANK_LEXICAL_MAX ||
       (qvec && dims <= 0)) {
      return FAILURE;
   }
   out->store = calloc(1, sizeof(*out->store));
   if (!out->store) {
      return FAILURE;
   }

   /* Keyword channel first: its hits' cosines then come from the semantic pass. */
   float hit_bm25[DOCUMENT_RANK_LEXICAL_MAX];
   float hit_cos[DOCUMENT_RANK_LEXICAL_MAX];
   const int n_hits = rank_keyword(user_id, query, opts, out->store, hit_bm25);
   for (int i = 0; i < n_hits; i++) {
      hit_cos[i] = NAN;
   }

   document_chunk_score_t top[DOCUMENT_RANK_MAX];
   int n_top = 0;
   int n_meta = 0;
   if (qvec && rank_semantic(user_id, qvec, dims, opts, out, n_hits, hit_cos, top, &n_top,
                             &n_meta) != SUCCESS) {
      OLOG_ERROR("document_rank: semantic ranking failed (user_id=%d)", user_id);
      document_ranking_free(out);
      return FAILURE;
   }

   out->items = calloc((size_t)(n_meta + n_hits) + 1, sizeof(*out->items));
   if (!out->items) {
      document_ranking_free(out);
      return FAILURE;
   }
   merge_candidates(out, top, n_top, n_meta, n_hits, hit_bm25, hit_cos);
   score_candidates(out, query, opts->temporal);
   add_phrase_bonus(user_id, out, query, opts);
   qsort(out->items, (size_t)out->count, sizeof(*out->items), cmp_hybrid);

   document_query_terms_t terms;
   document_query_terms(query, &terms);
   out->query_terms = terms.count;
   set_label_terms(out, &terms);
   return SUCCESS;
}
