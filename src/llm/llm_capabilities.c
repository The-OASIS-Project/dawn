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
 * Per-model reasoning capabilities and the mode/effort resolver.  See
 * llm_capabilities.h for the contract and models.toml [thinking.*] for the data.
 */

#include "llm/llm_capabilities.h"

#include <json-c/json.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "llm/llm_local_provider.h"
#include "llm/llm_model_family.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "toml.h"
#include "utils/string_utils.h"

/* A models.toml row: "prefix" = { modes = [...], efforts = [...] }. */
typedef struct {
   char *prefix; /* NULL-terminated array */
   int mode_count;
   llm_think_mode_t modes[LLM_THINK_MODES_MAX];
   int effort_count;
   char efforts[LLM_EFFORTS_MAX][LLM_EFFORT_NAME_MAX];
} caps_row_t;

/* Longest prefix first; loaded once at startup, read-only afterwards. */
static caps_row_t *s_anthropic_rows;
static caps_row_t *s_openai_rows;
static caps_row_t *s_gemini_rows;

/* Every effort name, lowest first: the order effort snapping measures in. */
static const char *const EFFORT_ORDER[] = { "none", "minimal", "low", "medium",
                                            "high", "xhigh",   "max" };
#define EFFORT_ORDER_COUNT ((int)(sizeof(EFFORT_ORDER) / sizeof(EFFORT_ORDER[0])))

/* DAWN's thinking budget sizes ([llm.thinking] budget_*): the levels of a
 * Claude or llama.cpp "enabled" mode.  llm_thinking_budget_size() maps them. */
static const char *const BUDGET_LEVELS[] = { "low", "medium", "high", "xhigh" };
#define BUDGET_LEVEL_COUNT ((int)(sizeof(BUDGET_LEVELS) / sizeof(BUDGET_LEVELS[0])))

static int effort_rank(const char *effort) {
   for (int i = 0; effort && i < EFFORT_ORDER_COUNT; i++) {
      if (strcmp(effort, EFFORT_ORDER[i]) == 0) {
         return i;
      }
   }
   return -1;
}

static bool parse_mode(const char *name, llm_think_mode_t *out) {
   if (strcmp(name, "disabled") == 0) {
      *out = LLM_THINK_DISABLED;
   } else if (strcmp(name, "adaptive") == 0) {
      *out = LLM_THINK_ADAPTIVE;
   } else if (strcmp(name, "enabled") == 0) {
      *out = LLM_THINK_ENABLED;
   } else {
      return false;
   }
   return true;
}

const char *llm_think_mode_name(llm_think_mode_t mode) {
   switch (mode) {
      case LLM_THINK_ADAPTIVE:
         return "adaptive";
      case LLM_THINK_ENABLED:
         return "enabled";
      case LLM_THINK_DISABLED:
      default:
         return "disabled";
   }
}

static int row_cmp_desc(const void *a, const void *b) {
   const size_t la = strlen(((const caps_row_t *)a)->prefix);
   const size_t lb = strlen(((const caps_row_t *)b)->prefix);
   return la < lb ? 1 : la > lb ? -1 : 0;
}

/* One row's modes and efforts; a malformed value is logged and skipped. */
static void parse_row(const char *provider,
                      const char *prefix,
                      toml_table_t *entry,
                      caps_row_t *row) {
   toml_array_t *modes = toml_array_in(entry, "modes");
   for (int i = 0; modes && i < toml_array_nelem(modes); i++) {
      toml_datum_t d = toml_string_at(modes, i);
      if (!d.ok) {
         continue;
      }
      llm_think_mode_t mode;
      if (row->mode_count < LLM_THINK_MODES_MAX && parse_mode(d.u.s, &mode)) {
         row->modes[row->mode_count++] = mode;
      } else {
         OLOG_WARNING("models.toml [thinking.%s] \"%s\": ignoring mode '%s'", provider, prefix,
                      d.u.s);
      }
      free(d.u.s);
   }
   toml_array_t *efforts = toml_array_in(entry, "efforts");
   for (int i = 0; efforts && i < toml_array_nelem(efforts); i++) {
      toml_datum_t d = toml_string_at(efforts, i);
      if (!d.ok) {
         continue;
      }
      if (row->effort_count < LLM_EFFORTS_MAX && effort_rank(d.u.s) >= 0) {
         snprintf(row->efforts[row->effort_count++], LLM_EFFORT_NAME_MAX, "%s", d.u.s);
      } else {
         OLOG_WARNING("models.toml [thinking.%s] \"%s\": ignoring effort '%s'", provider, prefix,
                      d.u.s);
      }
      free(d.u.s);
   }
}

static caps_row_t *load_rows(toml_table_t *thinking, const char *provider) {
   toml_table_t *tab = thinking ? toml_table_in(thinking, provider) : NULL;
   const int n = tab ? toml_table_ntab(tab) : 0;
   if (n <= 0) {
      return NULL;
   }
   caps_row_t *arr = calloc((size_t)n + 1, sizeof(*arr));
   if (!arr) {
      return NULL;
   }
   int count = 0;
   const char *key = NULL;
   for (int i = 0; (key = toml_key_in(tab, i)) != NULL && count < n; i++) {
      toml_table_t *entry = toml_table_in(tab, key);
      if (!entry || !*key) {
         continue;
      }
      arr[count].prefix = strdup(key);
      if (!arr[count].prefix) {
         continue;
      }
      parse_row(provider, key, entry, &arr[count]);
      count++;
   }
   if (count == 0) {
      free(arr);
      return NULL;
   }
   qsort(arr, (size_t)count, sizeof(*arr), row_cmp_desc);
   return arr;
}

static void free_rows(caps_row_t **arr) {
   if (!arr || !*arr) {
      return;
   }
   for (int i = 0; (*arr)[i].prefix; i++) {
      free((*arr)[i].prefix);
   }
   free(*arr);
   *arr = NULL;
}

/* Anthropic model-id prefixes that take a mid-conversation system message
 * (models.toml [mid_system]), and those that take a tool defined in one
 * ([inline_tools]); NULL-terminated, loaded once, read-only after. */
static char **s_mid_system;
static char **s_inline_tools;

static void free_thinking_rows(void) {
   free_rows(&s_anthropic_rows);
   free_rows(&s_openai_rows);
   free_rows(&s_gemini_rows);
}

static void free_prefixes(char ***list) {
   for (int i = 0; *list && (*list)[i]; i++) {
      free((*list)[i]);
   }
   free(*list);
   *list = NULL;
}

static void free_mid_system(void) {
   free_prefixes(&s_mid_system);
   free_prefixes(&s_inline_tools);
}

/* models.toml [<table>] anthropic = [prefix, ...] into @p out. */
static void load_prefixes(struct toml_table_t *root, const char *table_name, char ***out) {
   free_prefixes(out);
   toml_table_t *table = root ? toml_table_in(root, table_name) : NULL;
   toml_array_t *list = table ? toml_array_in(table, "anthropic") : NULL;
   const int n = list ? toml_array_nelem(list) : 0;
   if (n <= 0) {
      return;
   }
   *out = calloc((size_t)n + 1, sizeof(**out));
   if (!*out) {
      return;
   }
   int kept = 0;
   for (int i = 0; i < n; i++) {
      toml_datum_t d = toml_string_at(list, i);
      if (d.ok && d.u.s && d.u.s[0]) {
         (*out)[kept++] = d.u.s; /* owned now */
      } else if (d.ok) {
         free(d.u.s);
      }
   }
}

static bool has_prefix(char **list, const char *model) {
   for (int i = 0; model && list && list[i]; i++) {
      if (strncmp(model, list[i], strlen(list[i])) == 0) {
         return true;
      }
   }
   return false;
}

void llm_capabilities_load_registry(struct toml_table_t *root) {
   free_thinking_rows();
   toml_table_t *thinking = root ? toml_table_in(root, "thinking") : NULL;
   if (!thinking) {
      return;
   }
   s_anthropic_rows = load_rows(thinking, "anthropic");
   s_openai_rows = load_rows(thinking, "openai");
   s_gemini_rows = load_rows(thinking, "gemini");
}

void llm_capabilities_load_mid_system(struct toml_table_t *root) {
   load_prefixes(root, "mid_system", &s_mid_system);
}

bool llm_model_mid_system(const char *model) {
   return has_prefix(s_mid_system, model);
}

void llm_capabilities_load_inline_tools(struct toml_table_t *root) {
   load_prefixes(root, "inline_tools", &s_inline_tools);
}

bool llm_model_inline_tools(const char *model) {
   return has_prefix(s_inline_tools, model);
}

/* models.toml [max_request_images]: one row per vendor key.  Loaded once,
 * read-only after. */
typedef struct {
   char key[LLM_IMAGE_LIMIT_KEY_MAX];
   llm_image_limit_t limit;
} image_limit_row_t;

#define IMAGE_LIMIT_ROWS_MAX 16
static image_limit_row_t s_image_limits[IMAGE_LIMIT_ROWS_MAX];
static int s_image_limit_count;

void llm_capabilities_load_image_limits(struct toml_table_t *root) {
   s_image_limit_count = 0;
   toml_table_t *table = root ? toml_table_in(root, "max_request_images") : NULL;
   const char *key = NULL;
   for (int i = 0; table && (key = toml_key_in(table, i)) != NULL; i++) {
      toml_table_t *row = toml_table_in(table, key);
      toml_datum_t count = row ? toml_int_in(row, "count") : (toml_datum_t){ 0 };
      toml_datum_t bytes = row ? toml_int_in(row, "bytes") : (toml_datum_t){ 0 };
      if (!count.ok || !bytes.ok || count.u.i <= 0 || bytes.u.i <= 0 ||
          strlen(key) >= LLM_IMAGE_LIMIT_KEY_MAX || s_image_limit_count >= IMAGE_LIMIT_ROWS_MAX) {
         OLOG_WARNING("models.toml: [max_request_images] %s ignored (needs count and bytes > 0)",
                      key);
         continue;
      }
      image_limit_row_t *r = &s_image_limits[s_image_limit_count++];
      snprintf(r->key, sizeof(r->key), "%s", key);
      r->limit.count = count.u.i > INT_MAX ? INT_MAX : (int)count.u.i;
      r->limit.bytes = (int64_t)bytes.u.i;
   }
}

bool llm_capabilities_image_limit(const char *key, llm_image_limit_t *out) {
   for (int i = 0; key && i < s_image_limit_count; i++) {
      if (strcmp(s_image_limits[i].key, key) == 0) {
         *out = s_image_limits[i].limit;
         return true;
      }
   }
   return false;
}

void llm_capabilities_free_registry(void) {
   free_thinking_rows();
   free_mid_system();
   s_image_limit_count = 0;
}

static const caps_row_t *lookup(const caps_row_t *arr, const char *model) {
   for (int i = 0; arr && model && arr[i].prefix; i++) {
      if (strncmp(model, arr[i].prefix, strlen(arr[i].prefix)) == 0) {
         return &arr[i];
      }
   }
   return NULL;
}

static void add_mode(llm_thinking_caps_t *out,
                     llm_think_mode_t mode,
                     const char (*efforts)[LLM_EFFORT_NAME_MAX],
                     int effort_count,
                     bool budget) {
   if (out->mode_count >= LLM_THINK_MODES_MAX) {
      return;
   }
   llm_think_mode_caps_t *m = &out->modes[out->mode_count++];
   memset(m, 0, sizeof(*m));
   m->mode = mode;
   m->budget = budget;
   if (budget) {
      for (int i = 0; i < BUDGET_LEVEL_COUNT && i < LLM_EFFORTS_MAX; i++) {
         snprintf(m->efforts[m->effort_count++], LLM_EFFORT_NAME_MAX, "%s", BUDGET_LEVELS[i]);
      }
      return;
   }
   for (int i = 0; efforts && i < effort_count && i < LLM_EFFORTS_MAX; i++) {
      /* safe_strscpy evaluates dst once; its other uses are inside sizeof/typeof */
      // NOLINTNEXTLINE(bugprone-macro-repeated-side-effects)
      safe_strscpy(m->efforts[m->effort_count++], efforts[i]);
   }
}

static void from_row(const caps_row_t *row, llm_model_family_t family, llm_thinking_caps_t *out) {
   out->source = LLM_CAPS_ROW;
   for (int i = 0; i < row->mode_count; i++) {
      const llm_think_mode_t mode = row->modes[i];
      if (mode == LLM_THINK_DISABLED) {
         add_mode(out, mode, NULL, 0, false);
      } else if (mode == LLM_THINK_ENABLED && family == LLM_FAMILY_ANTHROPIC) {
         add_mode(out, mode, NULL, 0, true);
      } else {
         add_mode(out, mode, (const char(*)[LLM_EFFORT_NAME_MAX])row->efforts, row->effort_count,
                  false);
      }
   }
}

static const char LOW_MEDIUM_HIGH[3][LLM_EFFORT_NAME_MAX] = { "low", "medium", "high" };

/* A model with no models.toml row: its provider's conservative default.  No
 * "disabled" unless the family is known to take it, and no reasoning
 * parameters at all for an OpenAI or Gemini model DAWN doesn't know is a
 * reasoning model (an OpenAI-compatible endpoint serves anything). */
static void provider_default(llm_model_family_t family, llm_thinking_caps_t *out) {
   out->source = LLM_CAPS_PROVIDER_DEFAULT;
   switch (family) {
      case LLM_FAMILY_ANTHROPIC:
         add_mode(out, LLM_THINK_ADAPTIVE, LOW_MEDIUM_HIGH, 3, false);
         break;
      case LLM_FAMILY_OTHER: /* OpenRouter's other vendors: it ignores what doesn't apply */
         add_mode(out, LLM_THINK_DISABLED, NULL, 0, false);
         add_mode(out, LLM_THINK_ENABLED, LOW_MEDIUM_HIGH, 3, false);
         break;
      default:
         break; /* no reasoning control */
   }
}

void llm_thinking_caps(llm_type_t type,
                       cloud_provider_t provider,
                       const char *model,
                       llm_thinking_caps_t *out) {
   memset(out, 0, sizeof(*out));
   char id[96];
   const llm_model_family_t family = llm_model_route(type, provider, model, id, sizeof(id));
   const caps_row_t *row = NULL;
   switch (family) {
      case LLM_FAMILY_LOCAL: {
         /* llama.cpp (and generic servers): off, or a fixed budget; Ollama:
          * think on or off.  Before the provider is detected, on or off only:
          * no budget levels offered that may not apply. */
         const local_provider_t local = llm_local_get_provider();
         out->source = LLM_CAPS_LOCAL;
         add_mode(out, LLM_THINK_DISABLED, NULL, 0, false);
         add_mode(out, LLM_THINK_ENABLED, NULL, 0,
                  local != LOCAL_PROVIDER_OLLAMA && local != LOCAL_PROVIDER_UNKNOWN);
         return;
      }
      case LLM_FAMILY_ANTHROPIC:
         row = lookup(s_anthropic_rows, id);
         break;
      case LLM_FAMILY_OPENAI:
         row = lookup(s_openai_rows, id);
         break;
      case LLM_FAMILY_GEMINI:
         row = lookup(s_gemini_rows, id);
         break;
      default:
         break;
   }
   if (row) {
      from_row(row, family, out);
   } else {
      provider_default(family, out);
   }
}

static const llm_think_mode_caps_t *find_mode(const llm_thinking_caps_t *caps,
                                              llm_think_mode_t mode) {
   for (int i = 0; i < caps->mode_count; i++) {
      if (caps->modes[i].mode == mode) {
         return &caps->modes[i];
      }
   }
   return NULL;
}

/* The first mode that reasons (anything but "disabled"). */
static const llm_think_mode_caps_t *first_reasoning_mode(const llm_thinking_caps_t *caps) {
   for (int i = 0; i < caps->mode_count; i++) {
      if (caps->modes[i].mode != LLM_THINK_DISABLED) {
         return &caps->modes[i];
      }
   }
   return NULL;
}

/* The offered effort nearest @p wanted (ties go lower); "" if none offered. */
static const char *nearest_effort(const llm_think_mode_caps_t *m, const char *wanted) {
   if (m->effort_count == 0) {
      return "";
   }
   int want = effort_rank(wanted);
   if (want < 0) {
      want = effort_rank("medium");
   }
   int best = 0;
   int best_distance = EFFORT_ORDER_COUNT + 1;
   for (int i = 0; i < m->effort_count; i++) {
      int distance = effort_rank(m->efforts[i]) - want;
      distance = distance < 0 ? -distance : distance;
      if (distance < best_distance) {
         best = i;
         best_distance = distance;
      }
   }
   return m->efforts[best];
}

static void resolve(const llm_thinking_caps_t *caps,
                    const char *mode,
                    const char *effort,
                    bool utility,
                    llm_thinking_resolved_t *out) {
   memset(out, 0, sizeof(*out));
   out->mode = LLM_THINK_DISABLED;
   if (!caps || caps->mode_count == 0) {
      return; /* no reasoning control: nothing to send */
   }
   out->controllable = true;

   const bool wants_off = utility || !mode || !*mode || strcmp(mode, "disabled") == 0;
   const llm_think_mode_caps_t *chosen = NULL;
   bool lowest = utility;
   if (wants_off) {
      chosen = find_mode(caps, LLM_THINK_DISABLED);
      if (!chosen) {
         /* Can't be turned off: the gentlest reasoning it has. */
         chosen = first_reasoning_mode(caps);
         lowest = true;
         out->mode_clamped = !utility;
      }
   } else {
      llm_think_mode_t wanted = LLM_THINK_ENABLED;
      const bool named = parse_mode(mode, &wanted); /* "auto" and unknowns: any reasoning */
      chosen = named ? find_mode(caps, wanted) : NULL;
      if (!chosen) {
         chosen = first_reasoning_mode(caps);
         /* A named mode the model lacks is a change the user should hear
          * about, except "enabled", which the older clients send to mean
          * "reasoning on" (on an adaptive-only model, that's adaptive). */
         out->mode_clamped = named && wanted != LLM_THINK_ENABLED;
      }
      if (!chosen) {
         chosen = find_mode(caps, LLM_THINK_DISABLED); /* a model that can't reason */
         out->mode_clamped = true;
      }
   }
   if (!chosen) {
      out->controllable = false;
      return;
   }
   out->mode = chosen->mode;
   out->budget = chosen->budget;
   if (chosen->mode == LLM_THINK_DISABLED || chosen->effort_count == 0) {
      return;
   }
   const char *pick = lowest ? chosen->efforts[0] : nearest_effort(chosen, effort);
   snprintf(out->effort, sizeof(out->effort), "%s", pick);
   if (!lowest && effort && *effort && strcmp(effort, pick) != 0) {
      out->effort_clamped = true;
   }
}

void llm_thinking_resolve(const llm_thinking_caps_t *caps,
                          const char *mode,
                          const char *effort,
                          bool utility,
                          llm_thinking_resolved_t *out) {
   resolve(caps, mode, effort, utility, out);
   out->clamped = out->mode_clamped || out->effort_clamped;
}

void llm_thinking_resolve_current(llm_type_t type,
                                  cloud_provider_t provider,
                                  const char *model,
                                  llm_thinking_resolved_t *out) {
   /* A request needs the local provider's real rules (a budget or on/off):
    * detect it if nothing has yet (cached after the first probe). */
   if (type == LLM_LOCAL && llm_local_get_provider() == LOCAL_PROVIDER_UNKNOWN) {
      llm_local_detect_provider(g_config.llm.local.endpoint[0] ? g_config.llm.local.endpoint
                                                               : "http://127.0.0.1:8080");
   }
   llm_thinking_caps_t caps;
   llm_thinking_caps(type, provider, model, &caps);
   /* A tools-off call gets the model's cheapest setting, unless it asked for
    * reasoning (memory extraction's [memory] extraction_effort): then the
    * model's first reasoning mode at that effort. */
   const bool utility = llm_tools_suppressed();
   const char *utility_effort = utility ? llm_get_current_utility_effort() : "";
   if (utility_effort[0]) {
      llm_thinking_resolve(&caps, "auto", utility_effort, false, out);
      return;
   }
   llm_thinking_resolve(&caps, llm_get_current_thinking_mode(), llm_get_current_reasoning_effort(),
                        utility, out);
}

int llm_thinking_budget_size(const char *level) {
   if (level && strcmp(level, "low") == 0) {
      return g_config.llm.thinking.budget_low;
   }
   if (level && strcmp(level, "high") == 0) {
      return g_config.llm.thinking.budget_high;
   }
   if (level && strcmp(level, "xhigh") == 0) {
      return g_config.llm.thinking.budget_xhigh;
   }
   return g_config.llm.thinking.budget_medium;
}

static const char *source_name(llm_caps_source_t source) {
   switch (source) {
      case LLM_CAPS_ROW:
         return "row";
      case LLM_CAPS_LOCAL:
         return "local";
      case LLM_CAPS_PROVIDER_DEFAULT:
      default:
         return "provider_default";
   }
}

struct json_object *llm_thinking_caps_to_json(const llm_thinking_caps_t *caps) {
   json_object *obj = json_object_new_object();
   json_object *modes = json_object_new_array();
   if (!obj || !modes) {
      json_object_put(obj);
      json_object_put(modes);
      return NULL;
   }
   json_object_object_add(obj, "source", json_object_new_string(source_name(caps->source)));
   for (int i = 0; i < caps->mode_count; i++) {
      const llm_think_mode_caps_t *m = &caps->modes[i];
      json_object *mode = json_object_new_object();
      json_object *efforts = json_object_new_array();
      json_object_object_add(mode, "mode", json_object_new_string(llm_think_mode_name(m->mode)));
      for (int e = 0; e < m->effort_count; e++) {
         json_object_array_add(efforts, json_object_new_string(m->efforts[e]));
      }
      json_object_object_add(mode, "efforts", efforts);
      json_object_object_add(mode, "budget", json_object_new_boolean(m->budget));
      if (m->budget) {
         json_object *tokens = json_object_new_object();
         for (int e = 0; e < m->effort_count; e++) {
            json_object_object_add(tokens, m->efforts[e],
                                   json_object_new_int(llm_thinking_budget_size(m->efforts[e])));
         }
         json_object_object_add(mode, "budget_tokens", tokens);
      }
      json_object_array_add(modes, mode);
   }
   json_object_object_add(obj, "modes", modes);

   /* What a new conversation gets: the configured default, resolved here. */
   llm_thinking_resolved_t def;
   llm_thinking_resolve(caps, g_config.llm.thinking.mode, g_config.llm.thinking.reasoning_effort,
                        false, &def);
   json_object *dflt = json_object_new_object();
   json_object_object_add(dflt, "mode", json_object_new_string(llm_think_mode_name(def.mode)));
   json_object_object_add(dflt, "effort", json_object_new_string(def.effort));
   json_object_object_add(obj, "default", dflt);
   return obj;
}
