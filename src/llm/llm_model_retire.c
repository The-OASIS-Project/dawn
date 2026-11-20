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
 * Retired models and their replacements (see llm_model_retire.h).
 */

#include "llm/llm_model_retire.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "toml.h"

/* Rows models.toml can hold: the table only grows (a stored conversation may name
 * a long-retired model), with room for local additions. */
#define RETIRE_ROWS_MAX 256
/* Replacements followed when a replacement is itself retired. */
#define RETIRE_CHAIN_MAX 4

typedef struct {
   cloud_provider_t provider;
   char model[LLM_RETIRE_MODEL_MAX];
   char by[LLM_RETIRE_MODEL_MAX];
   time_t on;          /* 0: retired now */
   _Atomic bool noted; /* logged the first time it is replaced */
} retire_row_t;

static retire_row_t s_rows[RETIRE_ROWS_MAX];
static int s_count;

static const struct {
   const char *table;
   cloud_provider_t provider;
} k_tables[] = {
   { "anthropic", CLOUD_PROVIDER_CLAUDE },
   { "openai", CLOUD_PROVIDER_OPENAI },
   { "gemini", CLOUD_PROVIDER_GEMINI },
   { "openrouter", CLOUD_PROVIDER_OPENROUTER },
};

/* "YYYY-MM-DD" as midnight UTC; false when it isn't one. */
static bool parse_date(const char *s, time_t *out) {
   struct tm tm = { 0 };
   int y, m, d;
   char tail;
   if (sscanf(s, "%4d-%2d-%2d%c", &y, &m, &d, &tail) != 3 || m < 1 || m > 12 || d < 1 || d > 31) {
      return false;
   }
   tm.tm_year = y - 1900;
   tm.tm_mon = m - 1;
   tm.tm_mday = d;
   *out = timegm(&tm);
   struct tm back;
   /* A day the month doesn't have (2027-02-31) isn't quietly another date. */
   return *out != (time_t)-1 && gmtime_r(out, &back) && back.tm_year == y - 1900 &&
          back.tm_mon == m - 1 && back.tm_mday == d;
}

/* A row's keys other than by and on (a misspelled `on` must not mean "now"). */
static bool has_unknown_key(toml_table_t *row) {
   const char *key = NULL;
   for (int i = 0; (key = toml_key_in(row, i)) != NULL; i++) {
      if (strcmp(key, "by") != 0 && strcmp(key, "on") != 0) {
         return true;
      }
   }
   return false;
}

/* An OpenRouter "vendor/model" id's vendor length; 0 when it names no real vendor
 * (no slash, an empty vendor, or OpenRouter's own router, which picks any vendor). */
static size_t vendor_len(const char *id) {
   const char *slash = strchr(id, '/');
   size_t n = slash ? (size_t)(slash - id) : 0;
   if (n == 0 || (n == strlen("openrouter") && strncmp(id, "openrouter", n) == 0)) {
      return 0;
   }
   return n;
}

static void load_row(cloud_provider_t provider,
                     const char *table,
                     const char *model,
                     toml_table_t *row) {
   toml_datum_t by = toml_string_in(row, "by");
   toml_datum_t on = toml_string_in(row, "on");
   const char *why = NULL;
   time_t when = 0;
   if (!by.ok || by.u.s[0] == '\0') {
      why = "needs by = \"<model>\"";
   } else if (strlen(model) >= LLM_RETIRE_MODEL_MAX || strlen(by.u.s) >= LLM_RETIRE_MODEL_MAX) {
      why = "id too long";
   } else if (has_unknown_key(row)) {
      why = "only by and on are allowed";
   } else if (toml_table_in(row, "on") || toml_array_in(row, "on") ||
              (toml_raw_in(row, "on") && (!on.ok || !parse_date(on.u.s, &when)))) {
      why = "on must be a quoted date, \"YYYY-MM-DD\"";
   } else if (provider == CLOUD_PROVIDER_OPENROUTER &&
              (vendor_len(model) == 0 || vendor_len(model) != vendor_len(by.u.s) ||
               strncmp(model, by.u.s, vendor_len(model)) != 0)) {
      why = "its replacement is from another vendor";
   } else if (s_count >= RETIRE_ROWS_MAX) {
      why = "too many rows";
   }
   if (why) {
      OLOG_WARNING("models.toml: [retired.%s] %s ignored (%s)", table, model, why);
   } else {
      retire_row_t *r = &s_rows[s_count++];
      r->provider = provider;
      snprintf(r->model, sizeof(r->model), "%s", model);
      snprintf(r->by, sizeof(r->by), "%s", by.u.s);
      r->on = when;
      atomic_store(&r->noted, false);
   }
   if (by.ok) {
      free(by.u.s);
   }
   if (on.ok) {
      free(on.u.s);
   }
}

void llm_model_retire_load(toml_table_t *root) {
   s_count = 0;
   toml_table_t *retired = root ? toml_table_in(root, "retired") : NULL;
   for (size_t t = 0; retired && t < sizeof(k_tables) / sizeof(k_tables[0]); t++) {
      toml_table_t *table = toml_table_in(retired, k_tables[t].table);
      const char *model = NULL;
      for (int i = 0; table && (model = toml_key_in(table, i)) != NULL; i++) {
         toml_table_t *row = toml_table_in(table, model);
         if (row) {
            load_row(k_tables[t].provider, k_tables[t].table, model, row);
         } else {
            OLOG_WARNING("models.toml: [retired.%s] %s ignored (needs { by = ... })",
                         k_tables[t].table, model);
         }
      }
   }
}

/* Digits that start a snapshot suffix: a date or a numbered version ("-20250514",
 * "-0613", "-001"), never a model's own version ("claude-opus-4-8"). */
#define SNAPSHOT_DIGITS_MIN 3

/* @p model is @p retired itself or a snapshot of it. */
static bool matches(const char *model, const char *retired) {
   size_t n = strlen(retired);
   if (strncmp(model, retired, n) != 0) {
      return false;
   }
   const char *rest = model + n;
   if (*rest == '\0') {
      return true;
   }
   if (*rest != '-') {
      return false;
   }
   size_t digits = strspn(rest + 1, "0123456789");
   if (digits < SNAPSHOT_DIGITS_MIN) {
      return false;
   }
   return strspn(rest + 1, "0123456789-") == strlen(rest + 1);
}

/* The most specific row that has retired @p model by @p now (a snapshot's own row
 * over its family's), whatever the file's order. */
static retire_row_t *find(cloud_provider_t provider, const char *model, time_t now) {
   retire_row_t *best = NULL;
   for (int i = 0; i < s_count; i++) {
      retire_row_t *r = &s_rows[i];
      if (r->provider == provider && (r->on == 0 || now >= r->on) && matches(model, r->model) &&
          (!best || strlen(r->model) > strlen(best->model))) {
         best = r;
      }
   }
   return best;
}

const char *llm_model_replacement(cloud_provider_t provider, const char *model, time_t now) {
   if (!model || model[0] == '\0') {
      return NULL;
   }
   const char *out = NULL;
   for (int step = 0; step < RETIRE_CHAIN_MAX; step++) {
      retire_row_t *r = find(provider, out ? out : model, now);
      if (!r) {
         break;
      }
      if (!atomic_exchange(&r->noted, true)) {
         OLOG_INFO("LLM model %s is retired: using %s", r->model, r->by);
      }
      out = r->by;
   }
   return out;
}
