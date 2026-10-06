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
 * Uses shared auth_db handle via auth_db_internal.h prepared statements.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "memory/contacts_db.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

/* =============================================================================
 * LIKE Escaping
 *
 * Escapes %, _, and \ in user input before building LIKE patterns.
 * Uses backslash as escape character (matched with ESCAPE '\' in SQL).
 * ============================================================================= */

static void escape_like(const char *input, char *out, size_t out_len) {
   size_t j = 0;
   for (size_t i = 0; input[i] && j < out_len - 2; i++) {
      if (input[i] == '%' || input[i] == '_' || input[i] == '\\') {
         out[j++] = '\\';
      }
      out[j++] = input[i];
   }
   out[j] = '\0';
}

/* @p in as contacts_find compares names (CONTACTS_NAME_SQL over canonical
 * names, which are lowercase ASCII with UTF-8 kept): hyphens as spaces, dots
 * and apostrophes dropped, leading, trailing and repeated spaces removed. */
static void normalize_name(const char *in, char *out, size_t out_len) {
   size_t j = 0;
   bool space = true; /* drops leading spaces */
   for (size_t i = 0; in[i] && j < out_len - 1; i++) {
      const unsigned char c = (unsigned char)in[i];
      if (c == '.' || c == '\'') {
         continue; /* as CONTACTS_NAME_SQL drops them */
      }
      const bool is_space = c == ' ' || c == '\t' || c == '-';
      if (is_space && space) {
         continue;
      }
      out[j++] = is_space ? ' ' : (c >= 0x80 ? (char)c : (char)tolower(c));
      space = is_space;
   }
   while (j > 0 && out[j - 1] == ' ') {
      j--;
   }
   out[j] = '\0';
}

/* =============================================================================
 * Row Helper
 * ============================================================================= */

static void row_to_contact(sqlite3_stmt *st, contact_result_t *out) {
   out->match = CONTACT_MATCH_EXACT; /* contacts_find sets the real one */
   out->contact_id = sqlite3_column_int64(st, 0);
   out->entity_id = sqlite3_column_int64(st, 1);

   const char *name = (const char *)sqlite3_column_text(st, 2);
   snprintf(out->entity_name, sizeof(out->entity_name), "%s", name ? name : "");

   const char *canonical = (const char *)sqlite3_column_text(st, 3);
   snprintf(out->canonical_name, sizeof(out->canonical_name), "%s", canonical ? canonical : "");

   const char *ftype = (const char *)sqlite3_column_text(st, 4);
   snprintf(out->field_type, sizeof(out->field_type), "%s", ftype ? ftype : "");

   const char *val = (const char *)sqlite3_column_text(st, 5);
   snprintf(out->value, sizeof(out->value), "%s", val ? val : "");

   const char *lbl = (const char *)sqlite3_column_text(st, 6);
   snprintf(out->label, sizeof(out->label), "%s", lbl ? lbl : "");

   const char *photo = (const char *)sqlite3_column_text(st, 7);
   snprintf(out->photo_id, sizeof(out->photo_id), "%s", photo ? photo : "");
}

/* =============================================================================
 * CRUD Operations
 * ============================================================================= */

int contacts_find(int user_id,
                  const char *name,
                  const char *field_type,
                  contact_result_t *out,
                  int max_results,
                  int *count_out) {
   if (count_out)
      *count_out = 0;

   if (!name || !name[0] || max_results <= 0)
      return 0;

   /* Validate name length before escaping */
   if (strlen(name) > 200)
      return 0;

   /* The name as canonical names are stored: lowercase, single spaces. */
   char norm[256];
   normalize_name(name, norm, sizeof(norm));
   if (!norm[0])
      return 0;

   /* Escape LIKE metacharacters */
   char escaped[512];
   escape_like(norm, escaped, sizeof(escaped));

   char pattern[600];
   snprintf(pattern, sizeof(pattern), "%%%s%%", escaped);
   char word[600];
   snprintf(word, sizeof(word), "%% %s %%", escaped);

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_contacts_find;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, user_id);
   sqlite3_bind_text(st, 2, norm, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, pattern, -1, SQLITE_TRANSIENT);
   if (field_type && field_type[0]) {
      sqlite3_bind_text(st, 4, field_type, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_text(st, 4, "%", -1, SQLITE_STATIC);
   }
   sqlite3_bind_text(st, 5, word, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 6, max_results);

   int count = 0;
   int step_rc;
   while (count < max_results && (step_rc = sqlite3_step(st)) == SQLITE_ROW) {
      row_to_contact(st, &out[count]);
      const int q = sqlite3_column_int(st, 8);
      out[count].match = q == 0   ? CONTACT_MATCH_EXACT
                         : q == 1 ? CONTACT_MATCH_WORD
                                  : CONTACT_MATCH_PARTIAL;
      count++;
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();

   if (step_rc != SQLITE_DONE && step_rc != SQLITE_ROW) {
      OLOG_ERROR("contacts_find: sqlite3_step error: %s", sqlite3_errmsg(s_db.db));
      return 1;
   }

   if (count_out)
      *count_out = count;
   return 0;
}

int contacts_add(int user_id,
                 int64_t entity_id,
                 const char *field_type,
                 const char *value,
                 const char *label) {
   if (!field_type || !value || !value[0])
      return 1;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_contacts_add;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, user_id);
   sqlite3_bind_int64(st, 2, entity_id);
   sqlite3_bind_text(st, 3, field_type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, value, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 5, label ? label : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 6, (int64_t)time(NULL));
   /* Entity ownership check parameters */
   sqlite3_bind_int64(st, 7, entity_id);
   sqlite3_bind_int(st, 8, user_id);

   int rc = sqlite3_step(st);
   sqlite3_reset(st);

   int changes = sqlite3_changes(s_db.db);
   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("contacts_add: insert failed: %s", sqlite3_errmsg(s_db.db));
      return 1;
   }
   if (changes == 0) {
      OLOG_ERROR("contacts_add: entity %lld not owned by user %d", (long long)entity_id, user_id);
      return 1;
   }
   return 0;
}

int contacts_delete(int user_id, int64_t contact_id) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_contacts_delete;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, contact_id);
   sqlite3_bind_int(st, 2, user_id);

   int rc = sqlite3_step(st);
   sqlite3_reset(st);

   int changes = sqlite3_changes(s_db.db);
   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE || changes == 0)
      return 1;
   return 0;
}

int contacts_count(int user_id, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_contacts_count;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, user_id);

   int count = 0;
   if (sqlite3_step(st) == SQLITE_ROW) {
      count = sqlite3_column_int(st, 0);
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();

   if (count_out)
      *count_out = count;
   return 0;
}

int contacts_update(int user_id,
                    int64_t contact_id,
                    const char *field_type,
                    const char *value,
                    const char *label) {
   if (!field_type || !value || !value[0])
      return 1;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_contacts_update;
   sqlite3_reset(st);
   sqlite3_bind_text(st, 1, field_type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, label ? label : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 4, contact_id);
   sqlite3_bind_int(st, 5, user_id);

   int rc = sqlite3_step(st);
   sqlite3_reset(st);

   int changes = sqlite3_changes(s_db.db);
   if (rc != SQLITE_DONE || changes == 0) {
      OLOG_ERROR("contacts_update: update failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return 1;
   }

   AUTH_DB_UNLOCK();
   return 0;
}

int contacts_list(int user_id,
                  const char *field_type,
                  contact_result_t *out,
                  int max_results,
                  int offset,
                  int *count_out) {
   if (count_out)
      *count_out = 0;

   if (max_results <= 0)
      return 0;
   if (offset < 0)
      offset = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_contacts_list;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, user_id);

   if (field_type && field_type[0]) {
      sqlite3_bind_text(st, 2, field_type, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 3, field_type, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(st, 2);
      sqlite3_bind_null(st, 3);
   }
   sqlite3_bind_int(st, 4, max_results);
   sqlite3_bind_int(st, 5, offset);

   int count = 0;
   while (count < max_results && sqlite3_step(st) == SQLITE_ROW) {
      row_to_contact(st, &out[count]);
      count++;
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();

   if (count_out)
      *count_out = count;
   return 0;
}

int contacts_entity_aliases(int user_id,
                            int64_t entity_id,
                            char (*names)[64],
                            int max_names,
                            int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!names || max_names <= 0 || entity_id <= 0)
      return 0;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int count = 0;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT n.canonical_name FROM memory_entities e JOIN "
                               "memory_entities n ON n.user_id = e.user_id AND n.id <> e.id AND "
                               "(n.id = COALESCE(e.canonical_id, e.id) OR n.canonical_id = "
                               "COALESCE(e.canonical_id, e.id)) WHERE e.user_id = ? AND e.id = ? "
                               "LIMIT ?",
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, entity_id);
      sqlite3_bind_int(st, 3, max_names);
      while (count < max_names && (rc = sqlite3_step(st)) == SQLITE_ROW) {
         const char *name = (const char *)sqlite3_column_text(st, 0);
         snprintf(names[count++], 64, "%s", name ? name : "");
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
      OLOG_ERROR("contacts_entity_aliases: %s", sqlite3_errmsg(s_db.db));
      return 1;
   }
   if (count_out)
      *count_out = count;
   return 0;
}

int contacts_list_names(int user_id,
                        const char *field_type,
                        contact_result_t *out,
                        int max_results,
                        int offset,
                        int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max_results <= 0)
      return 0;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int count = 0;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT c.id, c.entity_id, e.name, n.canonical_name, c.field_type, c.value, c.label, "
       "e.photo_id FROM contacts c JOIN memory_entities e ON c.entity_id = e.id "
       "JOIN memory_entities n ON n.user_id = c.user_id AND (n.id = COALESCE(e.canonical_id, e.id) "
       "OR n.canonical_id = COALESCE(e.canonical_id, e.id)) "
       "WHERE c.user_id = ? AND (? IS NULL OR c.field_type = ?) "
       "ORDER BY c.id, n.id LIMIT ? OFFSET ?",
       -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      if (field_type) {
         sqlite3_bind_text(st, 2, field_type, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(st, 3, field_type, -1, SQLITE_TRANSIENT);
      } else {
         sqlite3_bind_null(st, 2);
         sqlite3_bind_null(st, 3);
      }
      sqlite3_bind_int(st, 4, max_results);
      sqlite3_bind_int(st, 5, offset);
      while (count < max_results && (rc = sqlite3_step(st)) == SQLITE_ROW) {
         row_to_contact(st, &out[count++]);
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
      OLOG_ERROR("contacts_list_names: %s", sqlite3_errmsg(s_db.db));
      return 1;
   }
   if (count_out)
      *count_out = count;
   return 0;
}
