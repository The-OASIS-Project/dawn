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
 * Content words of a text, stemmed: how retrieval tells that a query names
 * something (a document's label, an entity's name).
 */

#ifndef MEMORY_TERMS_H
#define MEMORY_TERMS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes of stemmed words kept from one text (a long message's words). */
#define MEMORY_TERMS_LINE_MAX 4096
/** Most distinct words kept from one text. */
#define MEMORY_TERMS_WORDS_MAX 512
/** Longest word taken (longer runs aren't words: ids, hashes, URLs). */
#define MEMORY_TERM_LEN 48

/** A text's distinct stemmed words, sorted for lookup.  About 8 KB. */
typedef struct {
   char line[MEMORY_TERMS_LINE_MAX]; /**< the words, NUL-separated */
   const char *word[MEMORY_TERMS_WORDS_MAX];
   int count;
} memory_terms_t;

/**
 * @brief The next word at or after @p p, or NULL at the end of the text
 *
 * A word is a run of letters and digits, ASCII or not ("résumé" is one word,
 * "Tax_Return_2024" three).  Punctuation and spaces outside ASCII (curly
 * quotes, dashes, the ellipsis, a no-break space) separate words like their
 * ASCII counterparts.  Everything that compares words uses this, so both
 * sides of a comparison split the same way.
 *
 * @return Where the word ends
 */
const char *memory_terms_next_word(const char *p, const char **start, size_t *len);

/**
 * @brief Byte @p i of word @p w, case-folded
 *
 * ASCII, and the Latin-1 capitals À–Þ (which fold by adding 0x20 to their
 * second byte), so folding never changes a word's length.
 */
unsigned char memory_terms_fold_at(const char *w, size_t i);

/**
 * @brief A text's distinct words, stemmed
 *
 * The whole text, up to MEMORY_TERMS_WORDS_MAX distinct words, so a name at
 * the end of a long message still counts.
 *
 * @param content_only Leave out function words ("the", "what", "about", …),
 *                     which say nothing about what is meant
 */
void memory_terms_from_text(const char *text, bool content_only, memory_terms_t *out);

/**
 * @brief A text's distinct stemmed words as one space-separated line
 *
 * The compact form for storing many (an entity's name, a document's label).
 * A line longer than @p size keeps the words that fit.
 *
 * @return Distinct words in @p out
 */
int memory_terms_stem_line(const char *text, bool content_only, char *out, size_t size);

/** How many words @p text has, function words and single letters included. */
int memory_terms_word_count(const char *text);

/** Whether stemmed @p word is among @p terms. */
bool memory_terms_has(const memory_terms_t *terms, const char *word);

/**
 * @brief How many of @p stem_line's words (a memory_terms_stem_line) are
 *        among @p terms
 */
int memory_terms_count_in(const memory_terms_t *terms, const char *stem_line);

/**
 * @brief Whether @p query names @p name, and how many of the name's content
 *        words it matched (0: not named)
 *
 *   - two or more content words ("Harbor Lane Relocation"): two must match;
 *   - a one-word name ("Quillon"): that word must;
 *   - one content word among other words ("The Quillmen", "X Corp", "Borra
 *     Borra"): the name must appear in @p query as a phrase, since its other
 *     words turn up scattered through any long message.
 *
 * @param terms          @p query's content words (memory_terms_from_text)
 * @param name_stems     @p name's content words (memory_terms_stem_line)
 * @param content_words  How many that is (0: never named)
 * @param words          @p name's word count (memory_terms_word_count)
 */
int memory_terms_names(const memory_terms_t *terms,
                       const char *query,
                       const char *name,
                       const char *name_stems,
                       int content_words,
                       int words);

/**
 * @brief Whether @p matched of @p of words is enough to say a text is named
 *
 * At least two of them, or all of fewer.  A count of words, so it means the
 * same thing in every user's data (a keyword score's scale depends on the
 * corpus).
 */
bool memory_terms_enough(int matched, int of);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_TERMS_H */
