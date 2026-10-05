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
 * Who an action goes to: a spoken or typed name (or a number, an address)
 * resolved against the user's contacts, without guessing.  A name that is
 * only part of a contact's, that sounds like another contact's (spoken), or
 * that the user never said comes back as a question, not a recipient.  Shared
 * by phone and email.
 */

#ifndef CONTACT_RESOLVE_H
#define CONTACT_RESOLVE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONTACT_RESOLVE_MAX_CANDIDATES 5
#define CONTACT_RESOLVE_VALUE_MAX 256
#define CONTACT_RESOLVE_NAME_MAX 64
#define CONTACT_RESOLVE_MEMORY_MAX 640 /* what memory says about a name no contact has */

typedef enum {
   CONTACT_FIELD_PHONE = 0,
   CONTACT_FIELD_EMAIL,
} contact_field_t;

typedef enum {
   CONTACT_RESOLVE_LITERAL = 0, /* a number / one address, as given */
   CONTACT_RESOLVE_UNIQUE,      /* one contact, certain */
   CONTACT_RESOLVE_CONFIRM,     /* one contact, to confirm with the user first (why) */
   CONTACT_RESOLVE_AMBIGUOUS,   /* several contacts or values: which one? */
   CONTACT_RESOLVE_SUGGEST,     /* no match; near-misses to offer */
   CONTACT_RESOLVE_BAD_VALUE,   /* not a valid number/address, or the contact's isn't */
   CONTACT_RESOLVE_NONE,        /* nothing */
} contact_resolve_kind_t;

/* Why a result asks (CONFIRM: is it this one?  AMBIGUOUS: which one?). */
typedef enum {
   CONTACT_CONFIRM_NONE = 0,
   CONTACT_CONFIRM_PARTIAL,        /* only part of the contact's name ("Chris" → Christine) */
   CONTACT_CONFIRM_NOT_SAID,       /* not a name, number or address the user said */
   CONTACT_CONFIRM_SOUNDS_LIKE,    /* AMBIGUOUS: spoken, and another contact sounds alike */
   CONTACT_CONFIRM_SEVERAL_VALUES, /* AMBIGUOUS: one person with several numbers/addresses */
   CONTACT_CONFIRM_CLOSEST,        /* no contact by that name; this one comes close */
} contact_confirm_why_t;

typedef struct {
   char name[CONTACT_RESOLVE_NAME_MAX];
   char value[CONTACT_RESOLVE_VALUE_MAX];
   char label[32];
} contact_candidate_t;

typedef struct {
   contact_resolve_kind_t kind;
   contact_confirm_why_t why;             /* CONFIRM, AMBIGUOUS */
   char value[CONTACT_RESOLVE_VALUE_MAX]; /* LITERAL, UNIQUE, CONFIRM */
   char name[CONTACT_RESOLVE_NAME_MAX];   /* UNIQUE, CONFIRM (empty for a literal) */
   char label[32];                        /* UNIQUE, CONFIRM */
   contact_candidate_t candidates[CONTACT_RESOLVE_MAX_CANDIDATES]; /* AMBIGUOUS, SUGGEST */
   int candidate_count;
   char memory[CONTACT_RESOLVE_MEMORY_MAX]; /* NONE, in a live turn: what memory says about
                                               the name ("my wife"), or empty */
} contact_resolve_t;

typedef struct {
   contact_field_t field;
   bool spoken;            /* sound-alikes: other contacts, and the user's words */
   const char *user_words; /* the user's own words ("" in a live turn with none;
                              NULL: no turn, not checked) */
} contact_resolve_opts_t;

/**
 * @brief Resolve @p input (a name, a number, an address) for an action
 *
 * A literal (a valid number; exactly one plain address) is taken as given
 * when the user said it or named its contact, else CONFIRM.  A name ("my
 * wife": "wife") resolves to one contact only when it is that contact's whole
 * name or whole words of it (or of an alias), has one value of the field (a
 * trailing label picks one: "Bob Smith mobile"), sounds like no other
 * contact (spoken), and the user named it: the words passed, a
 * word of its name that alone picks it, or one of its aliases.  Otherwise
 * CONFIRM (one contact; also the one near-miss when only one comes close),
 * AMBIGUOUS (several, or sound-alikes), SUGGEST (near-misses) or NONE.
 *
 * @param user_id User whose contacts are searched
 * @param input   What the action names
 * @param opts    Field, spoken, user words
 * @param out     Result (always set)
 */
void contact_resolve(int user_id,
                     const char *input,
                     const contact_resolve_opts_t *opts,
                     contact_resolve_t *out);

/**
 * @brief The question for anything but LITERAL/UNIQUE: which one, did you
 *        mean, is this who you meant; written for the model to ask the user
 *        (and to pass back the full name or the value they choose)
 */
void contact_resolve_question(const char *input,
                              contact_field_t field,
                              const contact_resolve_t *r,
                              char *buf,
                              size_t buf_size);

/** Whether @p value is exactly one email address (no list, no display name). */
bool contact_email_is_single(const char *value);

/* Exposed for tests: whether @p value (a number or address, as a whole) or
 * every word of @p input (spoken: or one that sounds like it) is in
 * @p words.  NULL @p words: true (nothing to check). */
bool contact_resolve_said(const char *words,
                          const char *input,
                          const char *value,
                          contact_field_t field,
                          bool spoken);

#ifdef __cplusplus
}
#endif

#endif /* CONTACT_RESOLVE_H */
