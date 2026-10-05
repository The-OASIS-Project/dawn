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
 * Contacts database — links contact info (email, phone) to memory entities.
 */

#ifndef CONTACTS_DB_H
#define CONTACTS_DB_H

#include <stdint.h>

/* A name as contacts_find compares it: hyphens as spaces, dots and
 * apostrophes dropped ("mary-jane" ~ "mary jane", "o'brien" ~ "obrien"). */
#define CONTACTS_NAME_SQL(col) "replace(replace(replace(" col ", '-', ' '), '.', ''), '''', '')"

/* contacts_find's query: a name's contacts, best match first: the exact name
 * (0), a whole word of it or of another name of the same person (1: the
 * person's entity tree, its head and every alias of it; never exact), a
 * substring (2).  ?1 user, ?2 the normalized name, ?3 '%name%', ?4 field type,
 * ?5 '% name %', ?6 limit. */
#define CONTACTS_SAME_PERSON_SQL                                                    \
   "a.user_id = ?1 AND a.id <> e.id AND (a.id = COALESCE(e.canonical_id, e.id) OR " \
   "a.canonical_id = COALESCE(e.canonical_id, e.id))"
// clang-format off
#define CONTACTS_FIND_SQL                                                                     \
   "SELECT c.id, c.entity_id, e.name, e.canonical_name, c.field_type, c.value, c.label, "     \
   "e.photo_id, "                                                                             \
   "CASE WHEN " CONTACTS_NAME_SQL("e.canonical_name") " = ?2 THEN 0 "                         \
   "WHEN (' ' || " CONTACTS_NAME_SQL("e.canonical_name") " || ' ') LIKE ?5 ESCAPE '\\' "      \
   "THEN 1 "                                                                                  \
   "WHEN EXISTS (SELECT 1 FROM memory_entities a WHERE " CONTACTS_SAME_PERSON_SQL " AND "      \
   "(' ' || " CONTACTS_NAME_SQL("a.canonical_name") " || ' ') LIKE ?5 ESCAPE '\\') "          \
   "THEN 1 ELSE 2 END AS quality "                                                            \
   "FROM contacts c JOIN memory_entities e ON c.entity_id = e.id "                            \
   "WHERE c.user_id = ?1 AND c.field_type LIKE ?4 AND ("                                      \
   CONTACTS_NAME_SQL("e.canonical_name") " LIKE ?3 ESCAPE '\\' OR EXISTS (SELECT 1 FROM "     \
   "memory_entities a WHERE " CONTACTS_SAME_PERSON_SQL " AND "                                \
   CONTACTS_NAME_SQL("a.canonical_name") " LIKE ?3 ESCAPE '\\')) "                            \
   "ORDER BY quality, e.name LIMIT ?6"
// clang-format on

/* How a contact's name matched what was asked for (contacts_find). */
typedef enum {
   CONTACT_MATCH_EXACT = 0, /* the whole name */
   CONTACT_MATCH_WORD,      /* whole word(s) of the name, or of an alias */
   CONTACT_MATCH_PARTIAL,   /* inside a word ("chris" in "christine") */
} contact_match_t;

typedef struct {
   int64_t contact_id;
   int64_t entity_id;
   char entity_name[64];
   char canonical_name[64];
   char field_type[16]; /* "email", "phone", "address" */
   char value[256];
   char label[32];        /* "work", "personal", "mobile" */
   char photo_id[32];     /* image store ID or empty */
   contact_match_t match; /* contacts_find only */
} contact_result_t;

/**
 * @brief Find contacts by entity name and optional field type.
 *
 * Matches the name (normalized like canonical names) inside the entity's
 * canonical name or an alias's, best first: exact, whole word, then partial
 * (each row's `match`), so an exact name is never cut by the limit.  Escapes
 * LIKE metacharacters (%, _, \) in the name parameter.
 *
 * @param user_id     User ID for isolation
 * @param name        Entity name to search for (fuzzy match)
 * @param field_type  Filter by type ("email", "phone", etc.) or NULL for all
 * @param out         Output array
 * @param max_results Maximum results to return
 * @param count_out   Output: number of results found (may be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on failure
 */
int contacts_find(int user_id,
                  const char *name,
                  const char *field_type,
                  contact_result_t *out,
                  int max_results,
                  int *count_out);

/**
 * @brief Add a contact info record for an entity.
 *
 * @param user_id    User ID
 * @param entity_id  Memory entity ID
 * @param field_type Type: "email", "phone", "address"
 * @param value      The contact value (e.g., email address)
 * @param label      Optional label: "work", "personal", etc.
 * @return SUCCESS (0) on success, FAILURE (1) on failure
 */
int contacts_add(int user_id,
                 int64_t entity_id,
                 const char *field_type,
                 const char *value,
                 const char *label);

/**
 * @brief Delete a contact record by ID.
 *
 * @param user_id    User ID (ownership check)
 * @param contact_id Contact record ID
 * @return SUCCESS (0) on success, FAILURE (1) on failure
 */
int contacts_delete(int user_id, int64_t contact_id);

/**
 * @brief List all contacts for a user, optionally filtered by type.
 *
 * @param user_id    User ID
 * @param field_type Filter by exact type ("email", "phone", "address") or NULL for all
 * @param out        Output array
 * @param max_results Maximum results
 * @param offset     Number of rows to skip (for pagination)
 * @param count_out  Output: number of results returned (may be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on failure
 */
int contacts_list(int user_id,
                  const char *field_type,
                  contact_result_t *out,
                  int max_results,
                  int offset,
                  int *count_out);

/**
 * @brief Count all contacts for a user.
 *
 * @param user_id   User ID
 * @param count_out Output: contact count (may be NULL)
 * @return SUCCESS (0) on success, FAILURE (1) on failure
 */
int contacts_count(int user_id, int *count_out);

/**
 * @brief The person's other names (canonical, as stored): the head of
 *        @p entity_id's entity tree and every alias of it ("Cris", "wife")
 * @return SUCCESS, FAILURE on a database error (*count_out 0)
 */
int contacts_entity_aliases(int user_id,
                            int64_t entity_id,
                            char (*names)[64],
                            int max_names,
                            int *count_out);

/**
 * @brief contacts_list, one row per name of each contact's person (its own
 *        and every name in its entity tree, in canonical_name): for matching
 *        a name against everything a person is called
 */
int contacts_list_names(int user_id,
                        const char *field_type,
                        contact_result_t *out,
                        int max_results,
                        int offset,
                        int *count_out);

/**
 * @brief Update an existing contact record.
 *
 * @param user_id    User ID (ownership check)
 * @param contact_id Contact record ID
 * @param field_type New field type ("email", "phone", "address")
 * @param value      New contact value
 * @param label      New label ("work", "personal", etc.) or NULL
 * @return SUCCESS (0) on success, FAILURE (1) on failure
 */
int contacts_update(int user_id,
                    int64_t contact_id,
                    const char *field_type,
                    const char *value,
                    const char *label);

#endif /* CONTACTS_DB_H */
