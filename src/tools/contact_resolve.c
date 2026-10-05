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
 * Who an action goes to, without guessing: see contact_resolve.h.
 */

#include "tools/contact_resolve.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/str_fuzzy.h"
#include "memory/contacts_db.h"
#include "memory/memory_fact_search.h"
#include "tools/phone_number.h"
#include "utils/string_utils.h"

/* A near-miss must score at least this (token overlap or edit distance) to be
 * offered as "did you mean". */
#define SUGGEST_THRESHOLD 50
/* Two names sound alike when their sound keys match and their spellings are
 * this close (edit ratio): Cris/Chris 80, Jon/John 75, Sean/Shawn 60, but not
 * Jane/John 25 or Kate/Kit 50, which share a key and no one confuses. */
#define SOUNDS_ALIKE_RATIO 55
/* A short form counts from this many letters ("Cris", not "Jo"). */
#define SHORT_FORM_MIN 4
/* How much of the address book a near-miss or sound-alike scan reads. */
#define SCAN_MAX 500
#define SCAN_PAGE 25
/* Matches contacts_find returns before ranking stops mattering. */
#define FIND_MAX 25
/* The user's words read: well past any dictated request. */
#define WORDS_MAX 160
#define WORD_LEN 64
#define ALIASES_MAX 8

typedef struct {
   char word[WORDS_MAX][WORD_LEN];
   char key[WORDS_MAX][WORD_LEN];
   int count;
} words_t;

static words_t *words_new(const char *text);

/* @p text's words: lowercase letters and digits ('@' . - _ + and apostrophes
 * kept inside a word, so an address stays one), each with its sound key. */
static void split_words(const char *text, words_t *w) {
   w->count = 0;
   const unsigned char *p = (const unsigned char *)(text ? text : "");
   while (*p && w->count < WORDS_MAX) {
      while (*p && !isalnum(*p) && *p < 0x80) {
         p++;
      }
      size_t n = 0;
      char *dst = w->word[w->count];
      while (*p && (isalnum(*p) || *p >= 0x80 || *p == '@' || *p == '.' || *p == '-' || *p == '_' ||
                    *p == '+' || *p == '\'')) {
         char c = (char)tolower(*p);
         if (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0x99) {
            c = '\''; /* a curly apostrophe (phone keyboards) */
            p += 2;
         }
         if (n < WORD_LEN - 1) {
            dst[n++] = c;
         }
         p++;
      }
      /* A word's trailing punctuation ("Chris." "bob's") isn't part of it. */
      while (n > 0 && (dst[n - 1] == '.' || dst[n - 1] == '-' || dst[n - 1] == '\'')) {
         n--;
      }
      if (n > 1 && dst[n - 1] == 's' && dst[n - 2] == '\'') {
         n -= 2;
      }
      dst[n] = '\0';
      if (n > 0) {
         str_phonetic_key(dst, w->key[w->count], WORD_LEN);
         w->count++;
      }
   }
}

static words_t *words_new(const char *text) {
   words_t *w = malloc(sizeof(*w));
   if (w) {
      split_words(text, w);
   }
   return w;
}

/* A name word a sound-alike can be: letters, three or more. */
static bool name_like(const char *word) {
   size_t n = 0;
   for (const char *p = word; *p; p++, n++) {
      if (isdigit((unsigned char)*p) || *p == '@') {
         return false;
      }
   }
   return n >= 3;
}

/* Two different words a listener could take for each other. */
static bool sounds_alike(const char *a, const char *a_key, const char *b, const char *b_key) {
   if (strcmp(a, b) == 0 || !name_like(a) || !name_like(b) || strlen(a_key) < 2 ||
       strlen(b_key) < 2) {
      return false;
   }
   if (strcmp(a_key, b_key) == 0) {
      return str_fuzzy_ratio(a, b) >= SOUNDS_ALIKE_RATIO;
   }
   /* A short form ("Chris" for Cristopher, "Mike" for Michael): one sound key
    * starts the other, from a word of four letters or more. */
   const bool a_short = strlen(a_key) < strlen(b_key);
   const char *shorter = a_short ? a : b;
   const char *short_key = a_short ? a_key : b_key;
   const char *long_key = a_short ? b_key : a_key;
   return strlen(shorter) >= SHORT_FORM_MIN && strncmp(short_key, long_key, strlen(short_key)) == 0;
}

/* Whether @p word (with @p key) is one of @p w's words, or, spoken, sounds
 * like one. */
static bool has_word(const words_t *w, const char *word, const char *key, bool spoken) {
   for (int i = 0; i < w->count; i++) {
      if (strcmp(w->word[i], word) == 0 ||
          (spoken && sounds_alike(w->word[i], w->key[i], word, key))) {
         return true;
      }
   }
   return false;
}

/* Words naming no one ("my wife" is "wife"). */
static const char *skip_filler(const char *input) {
   static const char *const fillers[] = { "my ", "the ", "our " };
   for (size_t i = 0; i < sizeof(fillers) / sizeof(fillers[0]); i++) {
      const size_t n = strlen(fillers[i]);
      if (strncasecmp(input, fillers[i], n) == 0 && input[n]) {
         return input + n;
      }
   }
   return input;
}

/* Whether a number the user said (any punctuation, at least 7 digits)
 * normalizes to @p value. */
static bool number_said(const char *words, const char *value) {
   const char *p = words;
   while (*p) {
      if (!isdigit((unsigned char)*p) && *p != '+') {
         p++;
         continue;
      }
      char raw[40];
      size_t n = 0, digits = 0;
      while (*p && n < sizeof(raw) - 1 &&
             (isdigit((unsigned char)*p) ||
              ((*p == ' ' || *p == '-' || *p == '.' || *p == '(' || *p == ')' || *p == '+') &&
               (isdigit((unsigned char)p[1]) || p[1] == '(' || p[1] == ' ')))) {
         digits += isdigit((unsigned char)*p) != 0;
         raw[n++] = *p++;
      }
      raw[n] = '\0';
      if (n == 0) {
         p++;
         continue;
      }
      if (digits >= 7) {
         char norm[CONTACT_RESOLVE_VALUE_MAX];
         phone_number_normalize(raw, norm, sizeof(norm));
         if (strcmp(norm, value) == 0) {
            return true;
         }
      }
   }
   return false;
}

/* Whether the user's words hold @p value as a whole address. */
static bool address_said(const char *words, const char *value) {
   const char *p = words;
   const size_t len = strlen(value);
   while (*p) {
      while (*p && (isspace((unsigned char)*p) || strchr(",;<>()\"'", *p))) {
         p++;
      }
      const char *start = p;
      while (*p && !isspace((unsigned char)*p) && !strchr(",;<>()\"'", *p)) {
         p++;
      }
      size_t n = (size_t)(p - start);
      while (n > 0 && (start[n - 1] == '.' || start[n - 1] == '!' || start[n - 1] == '?')) {
         n--;
      }
      if (n == len && n > 0 && strncasecmp(start, value, len) == 0) {
         return true;
      }
   }
   return false;
}

bool contact_resolve_said(const char *words,
                          const char *input,
                          const char *value,
                          contact_field_t field,
                          bool spoken) {
   if (!words) {
      return true; /* no turn: nothing to check against */
   }
   if (value && value[0] &&
       (field == CONTACT_FIELD_EMAIL ? address_said(words, value) : number_said(words, value))) {
      return true;
   }
   /* Every word the action named is the user's (spoken: or sounds like it). */
   words_t *said = words_new(words);
   words_t *asked = words_new(input ? skip_filler(input) : "");
   bool all = said && asked && asked->count > 0;
   for (int i = 0; all && i < asked->count; i++) {
      all = has_word(said, asked->word[i], asked->key[i], spoken);
   }
   free(said);
   free(asked);
   return all;
}

bool contact_email_is_single(const char *value) {
   if (!value) {
      return false;
   }
   const char *at = NULL;
   size_t n = 0;
   for (const char *p = value; *p; p++, n++) {
      const unsigned char c = (unsigned char)*p;
      /* No list, no display name: one address. */
      if (c <= ' ' || strchr(",;<>\"()\\:[]", c)) {
         return false;
      }
      if (c == '@') {
         if (at) {
            return false;
         }
         at = p;
      }
   }
   return at && at > value && strchr(at, '.') && at[1] != '.' && value[n - 1] != '.' && n < 255;
}

static bool phone_number_valid(const char *input) {
   const size_t len = strlen(input);
   if (len < 3 || len > 20) {
      return false;
   }
   for (size_t i = 0; i < len; i++) {
      const char c = input[i];
      if (c != '+' && c != '-' && c != ' ' && c != '(' && c != ')' && c != '.' &&
          !(c >= '0' && c <= '9')) {
         return false;
      }
   }
   return true;
}

/* A field's value in one form, for comparing and dialing/sending. */
static void normalize_value(contact_field_t field, const char *in, char *out, size_t out_len) {
   if (field == CONTACT_FIELD_PHONE) {
      phone_number_normalize(in, out, out_len);
   } else {
      str_fuzzy_tolower(out, in, out_len);
   }
}

/* A value usable as it is: a dialable number (3-20 characters with three
 * digits or more), one address. */
static bool value_ok(contact_field_t field, const char *value) {
   if (field == CONTACT_FIELD_EMAIL) {
      return contact_email_is_single(value);
   }
   size_t digits = 0;
   for (const char *p = value; *p; p++) {
      digits += (*p >= '0' && *p <= '9');
   }
   return digits >= 3 && strlen(value) <= 20;
}

static const char *field_type(contact_field_t field) {
   return field == CONTACT_FIELD_PHONE ? "phone" : "email";
}

static bool has_candidate(const contact_resolve_t *r, const char *value) {
   for (int i = 0; i < r->candidate_count; i++) {
      if (strcmp(r->candidates[i].value, value) == 0) {
         return true;
      }
   }
   return false;
}

static void candidate_from(contact_candidate_t *dst,
                           const contact_result_t *src,
                           contact_field_t field) {
   snprintf(dst->name, sizeof(dst->name), "%s", src->entity_name);
   normalize_value(field, src->value, dst->value, sizeof(dst->value));
   snprintf(dst->label, sizeof(dst->label), "%s", src->label);
}

/* Best of the token and edit-distance scores of @p name for the needle. */
static int near_score(const char *needle_lower, const char *name) {
   char lower[128];
   str_fuzzy_tolower(lower, name, sizeof(lower));
   int score = str_fuzzy_score(lower, needle_lower);
   if (score < SUGGEST_THRESHOLD) {
      const int ratio = str_fuzzy_ratio(lower, needle_lower);
      if (ratio > score) {
         score = ratio;
      }
   }
   return score;
}

/* No match: the contacts whose names come close, best first. */
static void suggest(int user_id, const char *input, contact_field_t field, contact_resolve_t *out) {
   char needle[128];
   str_fuzzy_tolower(needle, input, sizeof(needle));
   int scores[CONTACT_RESOLVE_MAX_CANDIDATES] = { 0 };
   contact_result_t page[SCAN_PAGE];
   for (int offset = 0; offset < SCAN_MAX; offset += SCAN_PAGE) {
      int got = 0;
      if (contacts_list(user_id, field_type(field), page, SCAN_PAGE, offset, &got) != 0 ||
          got <= 0) {
         break;
      }
      for (int i = 0; i < got; i++) {
         int score = near_score(needle, page[i].entity_name);
         if (score < SUGGEST_THRESHOLD &&
             strcmp(page[i].entity_name, page[i].canonical_name) != 0) {
            const int c = near_score(needle, page[i].canonical_name);
            score = c > score ? c : score;
         }
         char value[CONTACT_RESOLVE_VALUE_MAX];
         normalize_value(field, page[i].value, value, sizeof(value));
         if (score < SUGGEST_THRESHOLD || has_candidate(out, value)) {
            continue;
         }
         int pos = out->candidate_count;
         if (pos >= CONTACT_RESOLVE_MAX_CANDIDATES) {
            if (score <= scores[CONTACT_RESOLVE_MAX_CANDIDATES - 1]) {
               continue;
            }
            pos = CONTACT_RESOLVE_MAX_CANDIDATES - 1;
         } else {
            out->candidate_count++;
         }
         while (pos > 0 && scores[pos - 1] < score) {
            scores[pos] = scores[pos - 1];
            out->candidates[pos] = out->candidates[pos - 1];
            pos--;
         }
         scores[pos] = score;
         candidate_from(&out->candidates[pos], &page[i], field);
      }
      if (got < SCAN_PAGE) {
         break;
      }
   }
   out->kind = out->candidate_count > 0 ? CONTACT_RESOLVE_SUGGEST : CONTACT_RESOLVE_NONE;
   /* One near-miss is a yes-or-no: a misheard name ("Christopher Curzi")
    * can't be said again any better, so the answer can't be a name. */
   if (out->candidate_count == 1) {
      const contact_candidate_t *c = &out->candidates[0];
      snprintf(out->name, sizeof(out->name), "%s", c->name);
      snprintf(out->value, sizeof(out->value), "%s", c->value);
      snprintf(out->label, sizeof(out->label), "%s", c->label);
      out->candidate_count = 0;
      out->kind = value_ok(field, out->value) ? CONTACT_RESOLVE_CONFIRM : CONTACT_RESOLVE_BAD_VALUE;
      out->why = CONTACT_CONFIRM_CLOSEST;
   }
}

/* A name no contact has, often a relationship ("my wife"): what memory says
 * about it, so the model can tell who it means.  The model doesn't search on
 * its own when a tool says the name is unknown. */
#define MEMORY_FACTS 3
#define MEMORY_FACT_BYTES 180

static void what_memory_says(int user_id, const char *input, char *out, size_t out_len) {
   memory_fact_t facts[MEMORY_FACTS];
   float scores[MEMORY_FACTS];
   int n = 0;
   out[0] = '\0';
   if (user_id <= 0 ||
       memory_search_execute(user_id, input, 0, facts, scores, MEMORY_FACTS, &n) != 0 || n <= 0) {
      return;
   }
   size_t len = (size_t)snprintf(out, out_len, "Memory says:");
   for (int i = 0; i < n && len < out_len; i++) {
      utf8_truncate(facts[i].fact_text, MEMORY_FACT_BYTES);
      const int w = snprintf(out + len, out_len - len, " \"%s\"", facts[i].fact_text);
      if (w < 0 || (size_t)w >= out_len - len) {
         break;
      }
      len += (size_t)w;
   }
}

/* The contact a number or address belongs to (its row), if any. */
static bool owner_of(int user_id, contact_field_t field, const char *value, contact_result_t *out) {
   char want[CONTACT_RESOLVE_VALUE_MAX];
   normalize_value(field, value, want, sizeof(want)); /* addresses compare lowercase */
   contact_result_t page[SCAN_PAGE];
   for (int offset = 0; offset < SCAN_MAX; offset += SCAN_PAGE) {
      int got = 0;
      if (contacts_list(user_id, field_type(field), page, SCAN_PAGE, offset, &got) != 0 ||
          got <= 0) {
         break;
      }
      for (int i = 0; i < got; i++) {
         char v[CONTACT_RESOLVE_VALUE_MAX];
         normalize_value(field, page[i].value, v, sizeof(v));
         if (strcmp(v, want) == 0) {
            *out = page[i];
            return true;
         }
      }
      if (got < SCAN_PAGE) {
         break;
      }
   }
   return false;
}

/* Whether @p word alone picks this person: the user's "Lee" names Christine
 * Lee only when she is the one Lee (which of her numbers is the label's or
 * the number's business). */
static bool word_picks(int user_id, contact_field_t field, const char *word, int64_t entity_id) {
   contact_result_t rows[FIND_MAX];
   int count = 0;
   if (contacts_find(user_id, word, field_type(field), rows, FIND_MAX, &count) != 0 || count == 0 ||
       rows[0].match == CONTACT_MATCH_PARTIAL) {
      return false;
   }
   for (int i = 0; i < count && rows[i].match == rows[0].match; i++) {
      if (rows[i].entity_id != entity_id) {
         return false;
      }
   }
   return true;
}

/* Whether the user named this contact: the words the action passed, a word
 * of the contact's name that alone picks it, or one of its aliases ("wife"). */
static bool contact_said(int user_id,
                         const contact_resolve_opts_t *opts,
                         const char *input,
                         const char *name,
                         int64_t entity_id,
                         const char *value) {
   if (contact_resolve_said(opts->user_words, input, value, opts->field, opts->spoken)) {
      return true;
   }
   words_t *said = words_new(opts->user_words);
   words_t *named = words_new(name);
   bool yes = false;
   for (int i = 0; said && named && !yes && i < named->count; i++) {
      yes = strlen(named->word[i]) >= 3 && has_word(said, named->word[i], named->key[i], false) &&
            word_picks(user_id, opts->field, named->word[i], entity_id);
   }
   char aliases[ALIASES_MAX][64];
   int n = 0;
   if (!yes && said && contacts_entity_aliases(user_id, entity_id, aliases, ALIASES_MAX, &n) == 0) {
      for (int a = 0; !yes && a < n; a++) {
         words_t *alias = words_new(aliases[a]);
         bool all = alias && alias->count > 0;
         for (int i = 0; all && i < alias->count; i++) {
            all = has_word(said, alias->word[i], alias->key[i], false);
         }
         yes = all;
         free(alias);
      }
   }
   free(said);
   free(named);
   return yes;
}

/* Spoken: another contact whose name (any name of the person) could be what
 * was said: every word of the request matches one of its words or sounds like
 * it, one of them spelled differently ("Chris" with a Chris and a Cristopher,
 * or a Cris). */
static bool sounds_like_other(int user_id,
                              const char *input,
                              int64_t entity_id,
                              const char *value,
                              contact_field_t field,
                              contact_resolve_t *out) {
   words_t *asked = words_new(input);
   words_t *name = malloc(sizeof(*name));
   bool found = false;
   if (!asked || !name || asked->count == 0) {
      free(asked);
      free(name);
      return false;
   }
   contact_result_t page[SCAN_PAGE];
   for (int offset = 0; out->candidate_count < CONTACT_RESOLVE_MAX_CANDIDATES && offset < SCAN_MAX;
        offset += SCAN_PAGE) {
      int got = 0;
      if (contacts_list_names(user_id, field_type(field), page, SCAN_PAGE, offset, &got) != 0 ||
          got <= 0) {
         break;
      }
      for (int i = 0; out->candidate_count < CONTACT_RESOLVE_MAX_CANDIDATES && i < got; i++) {
         char v[CONTACT_RESOLVE_VALUE_MAX];
         normalize_value(field, page[i].value, v, sizeof(v));
         if (page[i].entity_id == entity_id || strcmp(v, value) == 0 || has_candidate(out, v)) {
            continue; /* the same person, or already listed */
         }
         split_words(page[i].canonical_name, name); /* each name of the person */
         bool all = true, differs = false;
         for (int a = 0; a < asked->count && all; a++) {
            bool match = false;
            for (int b = 0; b < name->count && !match; b++) {
               if (strcmp(asked->word[a], name->word[b]) == 0) {
                  match = true;
               } else if (sounds_alike(asked->word[a], asked->key[a], name->word[b],
                                       name->key[b])) {
                  match = true;
                  differs = true;
               }
            }
            all = match;
         }
         if (all && differs) {
            candidate_from(&out->candidates[out->candidate_count++], &page[i], field);
            found = true;
         }
      }
      if (got < SCAN_PAGE) {
         break;
      }
   }
   free(asked);
   free(name);
   return found;
}

/* The best-matching rows of @p input.  A trailing label ("Bob Smith
 * mobile") picks among one person's values; it never picks the person.
 * Several people, or a last word that isn't one of the person's labels:
 * nothing.  @p name receives the input without the label it used. */
static int find_rows(int user_id,
                     const char *input,
                     contact_field_t field,
                     contact_result_t *rows,
                     char *name,
                     size_t name_len) {
   snprintf(name, name_len, "%s", input);
   int count = 0;
   if (contacts_find(user_id, input, field_type(field), rows, FIND_MAX, &count) != 0) {
      count = 0;
   }
   const char *last = strrchr(input, ' ');
   if (count > 0 || !last || last == input || !last[1]) {
      return count;
   }
   char stem[256];
   size_t n = (size_t)(last - input);
   while (n > 0 && (input[n - 1] == ',' || input[n - 1] == ' ')) {
      n--;
   }
   if (n == 0 || n >= sizeof(stem)) {
      return 0;
   }
   memcpy(stem, input, n);
   stem[n] = '\0';
   if (contacts_find(user_id, stem, field_type(field), rows, FIND_MAX, &count) != 0 || count == 0) {
      return 0;
   }
   /* Only the best tier, and only one person: else which person is asked. */
   int best = 0;
   bool one_person = true;
   while (best < count && rows[best].match == rows[0].match) {
      one_person = one_person && rows[best].entity_id == rows[0].entity_id;
      best++;
   }
   /* The last word must be one of that person's labels; else it was part of
    * a name ("Cris Kursey"), and nothing matched. */
   int kept = 0;
   for (int i = 0; one_person && i < best; i++) {
      if (rows[i].label[0] && strcasecmp(rows[i].label, last + 1) == 0) {
         rows[kept++] = rows[i];
      }
   }
   if (kept == 0) {
      return 0;
   }
   snprintf(name, name_len, "%s", stem);
   return kept;
}

void contact_resolve(int user_id,
                     const char *input,
                     const contact_resolve_opts_t *opts,
                     contact_resolve_t *out) {
   if (!out) {
      return;
   }
   memset(out, 0, sizeof(*out));
   out->kind = CONTACT_RESOLVE_NONE;
   if (!input || !opts) {
      return;
   }
   while (*input == ' ') {
      input++;
   }
   if (!input[0]) {
      return;
   }
   const contact_field_t field = opts->field;

   /* A literal: a number, or exactly one address. */
   const bool looks_literal = field == CONTACT_FIELD_PHONE
                                  ? (input[0] == '+' || (input[0] >= '0' && input[0] <= '9'))
                                  : strchr(input, '@') != NULL;
   if (looks_literal) {
      const bool valid = field == CONTACT_FIELD_PHONE ? phone_number_valid(input)
                                                      : contact_email_is_single(input);
      if (valid) {
         if (field == CONTACT_FIELD_EMAIL) {
            snprintf(out->value, sizeof(out->value), "%s", input); /* as typed */
         } else {
            normalize_value(field, input, out->value, sizeof(out->value));
         }
         if (!value_ok(field, out->value)) {
            out->kind = CONTACT_RESOLVE_BAD_VALUE;
            return;
         }
         /* Whose it is: a contact the user named, or none. */
         contact_result_t owner;
         const bool known = owner_of(user_id, field, out->value, &owner);
         if (known) {
            snprintf(out->name, sizeof(out->name), "%s", owner.entity_name);
         }
         /* Its contact named as written: with no other contact to compare
          * with here, a sound-alike isn't enough. */
         contact_resolve_opts_t exact = *opts;
         exact.spoken = false;
         if (contact_resolve_said(opts->user_words, input, out->value, field, false) ||
             (known && contact_said(user_id, &exact, owner.entity_name, owner.entity_name,
                                    owner.entity_id, out->value))) {
            out->kind = CONTACT_RESOLVE_LITERAL;
         } else {
            out->kind = CONTACT_RESOLVE_CONFIRM;
            out->why = CONTACT_CONFIRM_NOT_SAID;
         }
         return;
      }
      if (field == CONTACT_FIELD_EMAIL) {
         out->kind = CONTACT_RESOLVE_BAD_VALUE;
         return;
      }
      /* A digit-led name ("7-Eleven") may still be a contact. */
   }

   const char *lookup = skip_filler(input);
   contact_result_t rows[FIND_MAX];
   char name_only[256];
   const int count = find_rows(user_id, lookup, field, rows, name_only, sizeof(name_only));
   if (count == 0) {
      if (looks_literal) {
         out->kind = CONTACT_RESOLVE_BAD_VALUE;
         return;
      }
      suggest(user_id, name_only, field, out);
      if (out->kind == CONTACT_RESOLVE_NONE && opts->user_words) {
         what_memory_says(user_id, input, out->memory, sizeof(out->memory));
      }
      return;
   }

   /* The best kind of match, and its distinct values. */
   const contact_match_t best = rows[0].match;
   bool one_person = true;
   for (int i = 0; i < count && rows[i].match == best; i++) {
      char value[CONTACT_RESOLVE_VALUE_MAX];
      normalize_value(field, rows[i].value, value, sizeof(value));
      one_person = one_person && rows[i].entity_id == rows[0].entity_id;
      if (!has_candidate(out, value) && out->candidate_count < CONTACT_RESOLVE_MAX_CANDIDATES) {
         candidate_from(&out->candidates[out->candidate_count++], &rows[i], field);
      }
   }
   if (out->candidate_count > 1) {
      out->kind = CONTACT_RESOLVE_AMBIGUOUS;
      out->why = one_person ? CONTACT_CONFIRM_SEVERAL_VALUES : CONTACT_CONFIRM_NONE;
      return;
   }
   const contact_candidate_t one = out->candidates[0];
   out->candidate_count = 0;
   snprintf(out->name, sizeof(out->name), "%s", one.name);
   snprintf(out->value, sizeof(out->value), "%s", one.value);
   snprintf(out->label, sizeof(out->label), "%s", one.label);
   if (!value_ok(field, out->value)) {
      out->kind = CONTACT_RESOLVE_BAD_VALUE;
      return;
   }
   /* Spoken: everyone it could be, the match first. */
   bool alike = false;
   if (opts->spoken && best != CONTACT_MATCH_PARTIAL) {
      out->candidates[0] = one;
      out->candidate_count = 1;
      alike = sounds_like_other(user_id, name_only, rows[0].entity_id, out->value, field, out);
      if (!alike) {
         out->candidate_count = 0;
      }
   }
   out->kind = CONTACT_RESOLVE_CONFIRM;
   if (best == CONTACT_MATCH_PARTIAL) {
      out->why = CONTACT_CONFIRM_PARTIAL;
   } else if (alike) {
      /* Which one, not yes or no: chosen by full name. */
      out->kind = CONTACT_RESOLVE_AMBIGUOUS;
      out->why = CONTACT_CONFIRM_SOUNDS_LIKE;
   } else if (!contact_said(user_id, opts, input, out->name, rows[0].entity_id, out->value)) {
      out->why = CONTACT_CONFIRM_NOT_SAID;
   } else {
      out->kind = CONTACT_RESOLVE_UNIQUE;
   }
}

static void append_candidates(const contact_resolve_t *r,
                              bool labels_only,
                              char *buf,
                              size_t buf_size) {
   size_t len = strnlen(buf, buf_size);
   for (int i = 0; i < r->candidate_count && len < buf_size; i++) {
      const contact_candidate_t *c = &r->candidates[i];
      int n;
      if (labels_only) {
         n = snprintf(buf + len, buf_size - len, "%s%s (%s)", i == 0 ? "" : "; ",
                      c->label[0] ? c->label : "no label", c->value);
      } else {
         n = snprintf(buf + len, buf_size - len, "%s%s (%s%s%s)", i == 0 ? "" : "; ", c->name,
                      c->value, c->label[0] ? ", " : "", c->label);
      }
      if (n < 0 || (size_t)n >= buf_size - len) {
         break;
      }
      len += (size_t)n;
   }
}

void contact_resolve_question(const char *input,
                              contact_field_t field,
                              const contact_resolve_t *r,
                              char *buf,
                              size_t buf_size) {
   if (!buf || buf_size == 0) {
      return;
   }
   const char *who = input && input[0] ? input : "that contact";
   const char *what = field == CONTACT_FIELD_PHONE ? "number" : "address";
   const char *a_what = field == CONTACT_FIELD_PHONE ? "a number" : "an address";
   if (!r) {
      snprintf(buf, buf_size, "Couldn't resolve '%s'.", who);
      return;
   }
   size_t len;
   switch (r->kind) {
      case CONTACT_RESOLVE_CONFIRM:
         switch (r->why) {
            case CONTACT_CONFIRM_CLOSEST:
               snprintf(buf, buf_size,
                        "No contact is named '%s'; the closest is %s (%s). Ask the user if that's "
                        "who they mean; don't assume it.",
                        who, r->name, r->value);
               break;
            case CONTACT_CONFIRM_PARTIAL:
               snprintf(buf, buf_size,
                        "'%s' is only part of a contact's name: I have %s (%s). Ask the user if "
                        "that's who they mean; don't assume it.",
                        who, r->name, r->value);
               break;
            default:
               if (r->name[0]) {
                  snprintf(buf, buf_size,
                           "%s (%s) isn't someone the user named. Check with the user before "
                           "going ahead.",
                           r->name, r->value);
               } else {
                  snprintf(buf, buf_size,
                           "%s isn't %s the user gave. Check with the user before going ahead.",
                           r->value, a_what);
               }
               break;
         }
         return;
      case CONTACT_RESOLVE_AMBIGUOUS:
         if (r->why == CONTACT_CONFIRM_SOUNDS_LIKE) {
            snprintf(buf, buf_size, "'%s' sounds like more than one contact: ", who);
            append_candidates(r, false, buf, buf_size);
            len = strnlen(buf, buf_size);
            snprintf(buf + len, buf_size - len,
                     ". Ask the user which one; then pass that contact's full name.");
         } else if (r->why == CONTACT_CONFIRM_SEVERAL_VALUES) {
            snprintf(buf, buf_size, "%s has more than one %s: ", r->candidates[0].name, what);
            append_candidates(r, true, buf, buf_size);
            len = strnlen(buf, buf_size);
            snprintf(buf + len, buf_size - len,
                     ". Ask the user which; then pass the name and its label "
                     "(\"%s mobile\"), or the %s.",
                     r->candidates[0].name, what);
         } else {
            snprintf(buf, buf_size, "Several contacts match '%s': ", who);
            append_candidates(r, false, buf, buf_size);
            len = strnlen(buf, buf_size);
            snprintf(buf + len, buf_size - len,
                     ". Ask the user which one, offering each %s listed; then pass that "
                     "contact's full name, with its label when that person has more than one "
                     "(\"Bob Smith mobile\"), or the %s.",
                     what, what);
         }
         return;
      case CONTACT_RESOLVE_SUGGEST:
         snprintf(buf, buf_size, "No contact is named '%s'. Did they mean: ", who);
         append_candidates(r, false, buf, buf_size);
         len = strnlen(buf, buf_size);
         snprintf(buf + len, buf_size - len,
                  "? Ask the user; don't pick one for them. Otherwise ask for the %s.", what);
         return;
      case CONTACT_RESOLVE_BAD_VALUE:
         if (r->name[0]) {
            snprintf(buf, buf_size, "The %s on file for %s isn't valid. Ask the user for it.", what,
                     r->name);
         } else {
            snprintf(buf, buf_size, "'%s' isn't a valid %s. Ask the user for it.", who,
                     field == CONTACT_FIELD_PHONE ? "phone number" : "single email address");
         }
         return;
      case CONTACT_RESOLVE_NONE:
         if (r->memory[0]) {
            snprintf(buf, buf_size,
                     "No contact named '%s'. %s If that tells you who the user means, call again "
                     "now with that person's name: the user is asked to confirm it, so don't "
                     "ask first. Otherwise ask the user for the %s, or save the contact first.",
                     who, r->memory, what);
         } else {
            snprintf(buf, buf_size,
                     "No contact named '%s'. Ask the user who they mean, or for the %s, or save "
                     "the contact first.",
                     who, what);
         }
         return;
      default:
         snprintf(buf, buf_size, "Couldn't resolve '%s'.", who);
         return;
   }
}
